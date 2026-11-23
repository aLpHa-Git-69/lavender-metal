/*
 * LavenderMetal: the plug-in Metal loads when the kernel service names it (M5.1, M5.2).
 *
 * How Metal finds a device (the system's own code, MTLRegisterDevices): it matches the IOKit class IOAcceleratorES, reads the
 * service's MetalPluginName ("LavenderMetal") and MetalPluginClassName ("LavenderMetalDevice"), opens
 * /System/Library/Extensions/LavenderMetal.bundle, takes that class, insists that it is a subclass of Metal's private
 * _MTLDevice that conforms to MTLDevice and MTLDeviceSPI, and sends it -initWithAcceleratorPort:.
 *
 * M5.2 adds the objects a program uses to move bytes with the GPU:
 *   buffers          memory the kernel service hands out (physically contiguous, below 4 GB, mapped into the process), so the
 *                    CPU sees it through -contents and the GPU addresses it through -gpuAddress
 *   command queue    -newCommandQueue, -commandBuffer
 *   command buffer   -blitCommandEncoder, -commit, -waitUntilCompleted, completed handlers, -status
 *   blit encoder     -fillBuffer:range:value: (the GPU's command processor writes the words: CP_MEM_WRITE) and
 *                    -copyFromBuffer:... (the 2D core blits the 64-byte aligned middle, the same CP_BLIT sequence as the kernel's
 *                    step 18; an unaligned head and tail are copied by the CPU and counted in the log)
 * A command buffer runs when it is committed, op by op, each op one synchronous submission to the service.
 *
 * Everything Metal sends that nothing implements is answered with zero and written once to /var/root/lavmetal.log
 * ("asked and not implemented"); the return type is read from the protocols the class declares, so structs come back sized.
 *
 * _MTLDevice and the other bases are not in the SDK (they live in the system's Metal.framework), so the classes are made at run
 * time, in the constructor that runs when the bundle is loaded. Built without ARC.
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LAV_LOG "/var/root/lavmetal.log"

static void
lavlog(const char * fmt, ...)
{
	char line[512];
	va_list ap;
	FILE * f;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	f = fopen(LAV_LOG, "a");
	if (f != NULL) {
		fprintf(f, "%ld LavenderMetal[%d]: %s\n", (long)time(NULL), (int)getpid(), line);
		fclose(f);
	}
	NSLog(@"LavenderMetal: %s", line);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* The service: LavenderGPU (kernel/LavenderGPU.cpp). Buffers, and streams of PM4 words submitted by reference.         */
/* ------------------------------------------------------------------------------------------------------------------ */

enum { kLavInfo = 0, kLavBufNew = 1, kLavBufFree = 2, kLavSubmit = 3, kLavSubmitBuf = 4, kLavMetal = 5 };

struct lav_buf_new { uint64_t size; uint32_t handle, pad; uint64_t address; };
struct lav_buf_free { uint32_t handle, pad; };
struct lav_submit_out { uint32_t fence, status; };
struct lav_submit_buf { uint32_t handle, offset, dwords, pad; };

struct lbuf {
	uint32_t handle;
	uint64_t gpu;                   /* the GPU's address (= physical, below 4 GB) */
	void * cpu;                     /* the process's mapping */
	size_t len;
	mach_vm_address_t map;
};

static io_connect_t gConn;
static struct lbuf gStream;         /* [0, 16 KB): the PM4 words of a submission; [32 KB, 33 KB): scratch the GPU writes events to */
static pthread_mutex_t gGPULock = PTHREAD_MUTEX_INITIALIZER;

#define STREAM_WORDS    4096u
#define SCRATCH_OFFSET  0x8000u

static BOOL
lavBufNew(size_t len, struct lbuf * b)
{
	struct lav_buf_new bn;
	size_t out = sizeof(bn);
	mach_vm_size_t msize = 0;
	kern_return_t kr;

	memset(&bn, 0, sizeof(bn));
	bn.size = len;
	kr = IOConnectCallStructMethod(gConn, kLavBufNew, &bn, sizeof(bn), &bn, &out);
	if (kr != KERN_SUCCESS) {
		lavlog("buffer of %zu bytes: kr %#x", len, kr);
		return NO;
	}
	b->handle = bn.handle;
	b->gpu = bn.address;
	b->len = (size_t)bn.size;
	b->map = 0;
	kr = IOConnectMapMemory64(gConn, bn.handle, mach_task_self(), &b->map, &msize, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		struct lav_buf_free bf = { bn.handle, 0 };

		lavlog("map of buffer %u: kr %#x", bn.handle, kr);
		IOConnectCallStructMethod(gConn, kLavBufFree, &bf, sizeof(bf), NULL, NULL);
		return NO;
	}
	b->cpu = (void *)(uintptr_t)b->map;
	return YES;
}

static void
lavBufFree(struct lbuf * b)
{
	struct lav_buf_free bf = { b->handle, 0 };

	if (b->handle == 0) {
		return;
	}
	IOConnectUnmapMemory64(gConn, b->handle, mach_task_self(), b->map);
	IOConnectCallStructMethod(gConn, kLavBufFree, &bf, sizeof(bf), NULL, NULL);
	memset(b, 0, sizeof(*b));
}

static BOOL
lavOpen(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
		kern_return_t kr;

		if (service == 0) {
			lavlog("no LavenderGPU service");
			return;
		}
		kr = IOServiceOpen(service, mach_task_self(), 0, &gConn);
		IOObjectRelease(service);
		if (kr != KERN_SUCCESS) {
			lavlog("IOServiceOpen: kr %#x", kr);
			gConn = 0;
			return;
		}
		if (!lavBufNew(65536, &gStream)) {
			gConn = 0;
			return;
		}
		lavlog("the GPU service is open; stream buffer at GPU %#llx", (unsigned long long)gStream.gpu);
	});
	return gConn != 0 && gStream.handle != 0;
}

/* One synchronous submission: the words go into the stream buffer and the service reads them from there. */
static BOOL
lavSubmit(const uint32_t * words, size_t count)
{
	struct lav_submit_buf sb = { gStream.handle, 0, (uint32_t)count, 0 };
	struct lav_submit_out out = { 0, 0 };
	size_t outsize = sizeof(out);
	kern_return_t kr;

	if (count == 0 || count > STREAM_WORDS) {
		return NO;
	}
	memcpy(gStream.cpu, words, count * 4);
	kr = IOConnectCallStructMethod(gConn, kLavSubmitBuf, &sb, sizeof(sb), &out, &outsize);
	if (kr != KERN_SUCCESS || out.fence == 0) {
		lavlog("submit of %zu words: kr %#x fence %u status %u", count, kr, out.fence, out.status);
		return NO;
	}
	return YES;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* PM4 streams                                                                                                         */
/* ------------------------------------------------------------------------------------------------------------------ */

static uint32_t
parity(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	return (0x9669u >> (v & 0xf)) & 1u;
}

struct stream {
	uint32_t w[STREAM_WORDS];
	size_t n;
};

static void
put(struct stream * s, uint32_t v)
{
	if (s->n < STREAM_WORDS) {
		s->w[s->n++] = v;
	}
}

static void
pkt7(struct stream * s, uint32_t opcode, uint32_t cnt)
{
	put(s, 0x70000000u | (cnt & 0x3fff) | (parity(cnt) << 15) | ((opcode & 0x7f) << 16) | (parity(opcode) << 23));
}

static void
pkt4(struct stream * s, uint32_t reg, uint32_t cnt)
{
	put(s, 0x40000000u | cnt | (parity(cnt) << 7) | ((reg & 0x3ffffu) << 8) | (parity(reg & 0x3ffffu) << 27));
}

static void
reg1(struct stream * s, uint32_t reg, uint32_t v)
{
	pkt4(s, reg, 1);
	put(s, v);
}

#define CP_MEM_WRITE            61u
#define CP_SET_RENDER_MODE      0x6cu
#define CP_BLIT                 0x2cu
#define CP_WAIT_FOR_IDLE        38u
#define CP_EVENT_WRITE          0x46u

/* a run of words written by the command processor at `gpu` (a multiple of 4), at most 1000 words a packet */
static void
emitWrite(struct stream * s, uint64_t gpu, const uint32_t * words, size_t count)
{
	while (count > 0) {
		size_t k = count > 1000 ? 1000 : count;

		pkt7(s, CP_MEM_WRITE, (uint32_t)(2 + k));
		put(s, (uint32_t)gpu);
		put(s, (uint32_t)(gpu >> 32));
		for (size_t i = 0; i < k; i++) {
			put(s, words[i]);
		}
		words += k;
		gpu += k * 4;
		count -= k;
	}
}

/* the 2D core's state, as the kernel's step 18 sets it (emit_setup) */
static void
emit2DSetup(struct stream * s)
{
	reg1(s, 0xe141u, 0x8);                  /* RB_RENDER_CNTL */
	reg1(s, 0x2100u, 0x86000000u);          /* RB_2D_BLIT_CNTL */
	reg1(s, 0x2180u, 0x86000000u);          /* GRAS_2D_BLIT_CNTL */
	reg1(s, 0x2184u, 0x9);
	reg1(s, 0xe140u, 1u << 17);             /* RB_CNTL: BYPASS */
	reg1(s, 0x0cc6u, 1u << 1);              /* RB_MODE_CNTL */
	reg1(s, 0x0ec3u, 0xc);                  /* SP_MODE_CNTL */
	reg1(s, 0x0e22u, 0x244u | (1u << 7));   /* TPL1_MODE_CNTL */
	reg1(s, 0x0e04u, 0x2);                  /* HLSQ_MODE_CNTL */
	reg1(s, 0xe000u, 0x181);                /* GRAS_CL_CNTL */
}

/* a linear byte copy as an R8 "image" of one row: n is a multiple of 64, src and dst 64-byte aligned */
static void
emitBlit(struct stream * s, uint64_t src, uint64_t dst, uint32_t n)
{
	uint32_t pitch = n >> 6;

	pkt7(s, CP_SET_RENDER_MODE, 1);
	put(s, 5);                              /* BLIT2D */
	pkt4(s, 0x2107u, 9);                    /* RB_2D_SRC_INFO .. */
	put(s, 0x3);
	put(s, (uint32_t)src);
	put(s, (uint32_t)(src >> 32));
	put(s, pitch | ((pitch * 2) << 16));
	for (int i = 0; i < 5; i++) {
		put(s, 0);
	}
	reg1(s, 0x2181u, 0x3);                  /* GRAS_2D_SRC_INFO */
	pkt4(s, 0x2110u, 9);                    /* RB_2D_DST_INFO .. */
	put(s, 0x3);
	put(s, (uint32_t)dst);
	put(s, (uint32_t)(dst >> 32));
	put(s, pitch | ((pitch * 2) << 16));
	for (int i = 0; i < 5; i++) {
		put(s, 0);
	}
	reg1(s, 0x2182u, 0x3);                  /* GRAS_2D_DST_INFO */
	pkt4(s, 0x2000u, 5);                    /* TPL1_A2D_SRC_TEXTURE_INFO .. */
	put(s, 0x3);
	put(s, n | (1u << 15));                 /* width n, height 1 */
	put(s, (uint32_t)src);
	put(s, (uint32_t)(src >> 32));
	put(s, pitch << 9);
	reg1(s, 0x2040u, 0x3u << 3);            /* SP_A2D_OUTPUT_INFO */
	pkt7(s, CP_BLIT, 5);
	put(s, 3);                              /* BLIT_OP_SCALE */
	put(s, 0);
	put(s, n - 1);
	put(s, 0);
	put(s, n - 1);
}

/* the end of a run of 2D blits: leave the 2D mode and push the colour cache out to memory */
static void
emit2DEnd(struct stream * s, uint32_t token)
{
	uint64_t flag = gStream.gpu + SCRATCH_OFFSET;

	pkt7(s, CP_SET_RENDER_MODE, 1);
	put(s, 8);                              /* END2D */
	pkt7(s, CP_WAIT_FOR_IDLE, 0);
	pkt7(s, CP_EVENT_WRITE, 4);
	put(s, 0x1du | (1u << 30));             /* PC_CCU_FLUSH_COLOR_TS */
	put(s, (uint32_t)flag);
	put(s, (uint32_t)(flag >> 32));
	put(s, token);
	pkt7(s, CP_WAIT_FOR_IDLE, 0);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Metal's classes. Protocol-driven signatures: a message nobody implements is logged once and answered with zeros.     */
/* ------------------------------------------------------------------------------------------------------------------ */

static Class gDeviceBase;
static id gDevice;
static unsigned int gPort;
static Protocol * gProtocols[96];
static unsigned gProtocolCount;
static Class gBufferClass, gQueueClass, gCommandBufferClass, gBlitClass;

static void
keepProtocol(Protocol * p)
{
	if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
		gProtocols[gProtocolCount++] = p;
	}
}

/* the protocol and Metal's private one of the same name (MTLBufferSPI ...), where this system has it */
static void
declareNamed(Class cls, const char * const * names)
{
	char spi[96];

	for (; *names != NULL; names++) {
		for (int private = 0; private < 2; private++) {
			Protocol * p;

			snprintf(spi, sizeof(spi), "%s%s", *names, private ? "SPI" : "");
			if ((p = objc_getProtocol(spi)) != NULL) {
				class_addProtocol(cls, p);
				keepProtocol(p);
			}
		}
	}
}

/* every protocol whose name begins with `prefix` (for the device and the queue, which have several private ones) */
static void
declareFamily(Class cls, const char * prefix)
{
	unsigned int count = 0;
	Protocol * __unsafe_unretained * all = objc_copyProtocolList(&count);

	for (unsigned int i = 0; i < count; i++) {
		if (strncmp(protocol_getName(all[i]), prefix, strlen(prefix)) == 0) {
			class_addProtocol(cls, all[i]);
			keepProtocol(all[i]);
		}
	}
	free(all);
}

static id
stubSignature(id self, SEL _cmd, SEL selector)
{
	for (unsigned i = 0; i < gProtocolCount; i++) {
		for (int required = 1; required >= 0; required--) {
			struct objc_method_description d = protocol_getMethodDescription(gProtocols[i], selector, required, YES);

			if (d.name != NULL && d.types != NULL) {
				return ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSMethodSignature"),
				    sel_registerName("signatureWithObjCTypes:"), d.types);
			}
		}
	}
	struct objc_super sup = { self, class_getSuperclass(object_getClass(self)) };

	(void)_cmd;
	return ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, sel_registerName("methodSignatureForSelector:"), selector);
}

static void
stubForward(id self, SEL _cmd, id invocation)
{
	static NSMutableSet * seen;
	SEL selector = ((SEL (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("selector"));
	id signature = ((id (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("methodSignature"));
	NSUInteger length = ((NSUInteger (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnLength"));
	NSString * name = NSStringFromSelector(selector);

	(void)_cmd;
	@synchronized(gDeviceBase) {
		if (seen == nil) {
			seen = [NSMutableSet new];
		}
		if (![seen containsObject:name]) {
			[seen addObject:name];
			lavlog("asked and not implemented: -%s (%s; returns %lu bytes, answered with zero)", [name UTF8String],
			    class_getName(object_getClass(self)), (unsigned long)length);
		}
	}
	if (length != 0) {
		void * zero = calloc(1, length);

		((void (*)(id, SEL, void *))objc_msgSend)(invocation, sel_registerName("setReturnValue:"), zero);
		free(zero);
	}
}

static void
addStubForwarding(Class cls)
{
	class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)stubSignature, "@@::");
	class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)stubForward, "v@:@");
}

/* each of our objects carries a block of state after its instance variables */
#define STATE(self, type) ((type *)object_getIndexedIvars(self))

static id
makeObject(Class cls, size_t extra)
{
	id o = class_createInstance(cls, extra);

	if (o != nil) {
		o = ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName("init"));
	}
	return o;
}

static id
objDevice(__unused id self, __unused SEL _cmd)
{
	return gDevice;
}

/* ---- buffers ---- */

struct bufstate {
	struct lbuf b;
	uint64_t options;
	id label;
};

static id
deviceNewBuffer(__unused id self, __unused SEL _cmd, uint64_t length, uint64_t options)
{
	id o;
	struct bufstate * st;
	BOOL ok;

	if (length == 0 || length > (16ull << 20) || !lavOpen()) {
		lavlog("newBufferWithLength:%llu refused", (unsigned long long)length);
		return nil;
	}
	o = makeObject(gBufferClass, sizeof(struct bufstate));
	if (o == nil) {
		return nil;
	}
	st = STATE(o, struct bufstate);
	memset(st, 0, sizeof(*st));
	pthread_mutex_lock(&gGPULock);
	ok = lavBufNew((size_t)length, &st->b);
	pthread_mutex_unlock(&gGPULock);
	if (!ok) {
		return nil;
	}
	memset(st->b.cpu, 0, st->b.len);
	st->options = options;
	return o;
}

static id
deviceNewBufferBytes(id self, SEL _cmd, const void * bytes, uint64_t length, uint64_t options)
{
	id o = deviceNewBuffer(self, _cmd, length, options);

	if (o != nil && bytes != NULL) {
		memcpy(STATE(o, struct bufstate)->b.cpu, bytes, (size_t)length);
	}
	return o;
}

static void
bufferDealloc(id self, __unused SEL _cmd)
{
	struct bufstate * st = STATE(self, struct bufstate);
	struct objc_super sup = { self, class_getSuperclass(gBufferClass) };

	pthread_mutex_lock(&gGPULock);
	lavBufFree(&st->b);
	pthread_mutex_unlock(&gGPULock);
	[st->label release];
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static void *
bufferContents(id self, __unused SEL _cmd)
{
	return STATE(self, struct bufstate)->b.cpu;
}

static uint64_t
bufferLength(id self, __unused SEL _cmd)
{
	return STATE(self, struct bufstate)->b.len;
}

static uint64_t
bufferOptions(id self, __unused SEL _cmd)
{
	return STATE(self, struct bufstate)->options;
}

static uint64_t
bufferStorageMode(id self, __unused SEL _cmd)
{
	return (STATE(self, struct bufstate)->options >> 4) & 0xf;
}

static uint64_t
bufferCacheMode(id self, __unused SEL _cmd)
{
	return STATE(self, struct bufstate)->options & 0xf;
}

static uint64_t
bufferHazardMode(id self, __unused SEL _cmd)
{
	return (STATE(self, struct bufstate)->options >> 8) & 0x3;
}

static uint64_t
bufferGPUAddress(id self, __unused SEL _cmd)
{
	return STATE(self, struct bufstate)->b.gpu;
}

static void
bufferDidModify(__unused id self, __unused SEL _cmd, __unused NSRange r)
{
}

static uint64_t
bufferPurgeable(__unused id self, __unused SEL _cmd, __unused uint64_t state)
{
	return 1;                               /* MTLPurgeableStateNonVolatile */
}

static id
bufferLabel(id self, __unused SEL _cmd)
{
	return STATE(self, struct bufstate)->label;
}

static void
bufferSetLabel(id self, __unused SEL _cmd, id label)
{
	struct bufstate * st = STATE(self, struct bufstate);

	[st->label release];
	st->label = [label copy];
}

/* ---- queue, command buffer, blit encoder ---- */

enum opkind { OP_FILL, OP_COPY };

struct op {
	enum opkind kind;
	id src, dst;                    /* retained buffers */
	uint64_t srcOff, dstOff, size;
	uint8_t value;
};

struct cbstate {
	id queue;
	NSMutableArray * handlers;
	NSMutableArray * scheduled;
	struct op * ops;
	size_t nops, cap;
	int status;                     /* MTLCommandBufferStatus */
	id label;
};

struct encstate {
	id commandBuffer;
	BOOL ended;
	id label;
};

static id
deviceNewQueue(__unused id self, __unused SEL _cmd)
{
	if (!lavOpen()) {
		return nil;
	}
	return makeObject(gQueueClass, 0);
}

static id
deviceNewQueueMax(id self, SEL _cmd, __unused uint64_t max)
{
	return deviceNewQueue(self, _cmd);
}

static id
queueCommandBuffer(id self, __unused SEL _cmd)
{
	id o = makeObject(gCommandBufferClass, sizeof(struct cbstate));
	struct cbstate * st;

	if (o == nil) {
		return nil;
	}
	st = STATE(o, struct cbstate);
	memset(st, 0, sizeof(*st));
	st->queue = [self retain];
	st->handlers = [NSMutableArray new];
	st->scheduled = [NSMutableArray new];
	st->status = 1;                         /* Enqueued */
	return o;
}

static id
queueLabel(__unused id self, __unused SEL _cmd)
{
	return nil;
}

static void
cbFreeOps(struct cbstate * st)
{
	for (size_t i = 0; i < st->nops; i++) {
		[st->ops[i].src release];
		[st->ops[i].dst release];
	}
	free(st->ops);
	st->ops = NULL;
	st->nops = st->cap = 0;
}

static void
cbDealloc(id self, __unused SEL _cmd)
{
	struct cbstate * st = STATE(self, struct cbstate);
	struct objc_super sup = { self, class_getSuperclass(gCommandBufferClass) };

	cbFreeOps(st);
	[st->queue release];
	[st->handlers release];
	[st->scheduled release];
	[st->label release];
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static struct op *
cbAddOp(struct cbstate * st)
{
	if (st->nops == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 16;
		st->ops = realloc(st->ops, st->cap * sizeof(struct op));
	}
	memset(&st->ops[st->nops], 0, sizeof(struct op));
	return &st->ops[st->nops++];
}

static id
cbBlitEncoder(id self, __unused SEL _cmd)
{
	id e = makeObject(gBlitClass, sizeof(struct encstate));
	struct encstate * es;

	if (e == nil) {
		return nil;
	}
	es = STATE(e, struct encstate);
	memset(es, 0, sizeof(*es));
	es->commandBuffer = [self retain];
	return e;
}

static void
encDealloc(id self, __unused SEL _cmd)
{
	struct encstate * es = STATE(self, struct encstate);
	struct objc_super sup = { self, class_getSuperclass(gBlitClass) };

	[es->commandBuffer release];
	[es->label release];
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static void
encFill(id self, __unused SEL _cmd, id buffer, NSRange range, uint8_t value)
{
	struct cbstate * st = STATE(STATE(self, struct encstate)->commandBuffer, struct cbstate);
	struct op * op = cbAddOp(st);

	op->kind = OP_FILL;
	op->dst = [buffer retain];
	op->dstOff = range.location;
	op->size = range.length;
	op->value = value;
}

static void
encCopy(id self, __unused SEL _cmd, id src, uint64_t srcOff, id dst, uint64_t dstOff, uint64_t size)
{
	struct cbstate * st = STATE(STATE(self, struct encstate)->commandBuffer, struct cbstate);
	struct op * op = cbAddOp(st);

	op->kind = OP_COPY;
	op->src = [src retain];
	op->dst = [dst retain];
	op->srcOff = srcOff;
	op->dstOff = dstOff;
	op->size = size;
}

static void
encEnd(id self, __unused SEL _cmd)
{
	STATE(self, struct encstate)->ended = YES;
}

static void
encNothing(__unused id self, __unused SEL _cmd, __unused id resource)
{
	/* synchronizeResource: the memory is the CPU's and the GPU's both; there is nothing to copy back */
}

static id
encLabel(id self, __unused SEL _cmd)
{
	return STATE(self, struct encstate)->label;
}

static void
encSetLabel(id self, __unused SEL _cmd, id label)
{
	struct encstate * es = STATE(self, struct encstate);

	[es->label release];
	es->label = [label copy];
}

/* run one op: the GPU does what it can, the CPU the unaligned edges */
static BOOL
runFill(struct op * op)
{
	struct bufstate * d = STATE(op->dst, struct bufstate);
	uint8_t * cpu = (uint8_t *)d->b.cpu + op->dstOff;
	uint64_t gpu = d->b.gpu + op->dstOff;
	uint64_t size = op->size;
	uint64_t head = (4 - (gpu & 3)) & 3;
	uint64_t body, tail;
	uint32_t pattern = op->value * 0x01010101u;

	if (op->dstOff + size > d->b.len) {
		lavlog("fill beyond the buffer (%llu + %llu > %zu)", (unsigned long long)op->dstOff, (unsigned long long)size, d->b.len);
		return NO;
	}
	if (head > size) {
		head = size;
	}
	body = (size - head) & ~3ull;
	tail = size - head - body;
	if (head != 0 || tail != 0) {
		lavlog("fill: %llu head and %llu tail bytes by the CPU", (unsigned long long)head, (unsigned long long)tail);
	}
	memset(cpu, op->value, (size_t)head);
	if (body != 0) {
		struct stream * s = calloc(1, sizeof(*s));
		uint32_t words[1000];

		for (size_t i = 0; i < 1000; i++) {
			words[i] = pattern;
		}
		for (uint64_t done = 0; done < body;) {
			s->n = 0;
			while (done < body && s->n + 1010 < STREAM_WORDS) {
				size_t k = (size_t)((body - done) / 4);

				if (k > 1000) {
					k = 1000;
				}
				emitWrite(s, gpu + head + done, words, k);
				done += k * 4;
			}
			if (!lavSubmit(s->w, s->n)) {
				free(s);
				return NO;
			}
		}
		free(s);
	}
	memset(cpu + head + body, op->value, (size_t)tail);
	return YES;
}

#define BLIT_CHUNK 4096u

static BOOL
runCopy(struct op * op)
{
	struct bufstate * sb = STATE(op->src, struct bufstate);
	struct bufstate * db = STATE(op->dst, struct bufstate);
	uint8_t * scpu = (uint8_t *)sb->b.cpu + op->srcOff;
	uint8_t * dcpu = (uint8_t *)db->b.cpu + op->dstOff;
	uint64_t sgpu = sb->b.gpu + op->srcOff;
	uint64_t dgpu = db->b.gpu + op->dstOff;
	uint64_t size = op->size, head = 0, body = 0, tail = 0;

	if (op->srcOff + size > sb->b.len || op->dstOff + size > db->b.len) {
		lavlog("copy beyond a buffer");
		return NO;
	}
	if (((sgpu ^ dgpu) & 63) == 0) {
		head = (64 - (dgpu & 63)) & 63;
		if (head > size) {
			head = size;
		}
		body = (size - head) & ~63ull;
	}
	tail = size - head - body;
	if (body == 0) {
		memmove(dcpu, scpu, (size_t)size);
		lavlog("copy of %llu bytes entirely by the CPU (too small or misaligned)", (unsigned long long)size);
		return YES;
	}
	if (head != 0 || tail != 0) {
		lavlog("copy: %llu head and %llu tail bytes by the CPU", (unsigned long long)head, (unsigned long long)tail);
	}
	memmove(dcpu, scpu, (size_t)head);
	{
		struct stream * s = calloc(1, sizeof(*s));
		uint64_t done = 0;
		BOOL ok = YES;

		while (done < body && ok) {
			s->n = 0;
			emit2DSetup(s);
			while (done < body && s->n + 140 < STREAM_WORDS) {
				uint32_t n = (uint32_t)((body - done) > BLIT_CHUNK ? BLIT_CHUNK : (body - done));

				emitBlit(s, sgpu + head + done, dgpu + head + done, n);
				done += n;
			}
			emit2DEnd(s, 0x2d2d0000u | (uint32_t)(done >> 6));
			ok = lavSubmit(s->w, s->n);
		}
		free(s);
		if (!ok) {
			return NO;
		}
	}
	memmove(dcpu + head + body, scpu + head + body, (size_t)tail);
	return YES;
}

typedef void (^LavHandler)(id);

static void
cbCommit(id self, __unused SEL _cmd)
{
	struct cbstate * st = STATE(self, struct cbstate);
	BOOL ok = YES;

	if (st->status >= 2) {
		return;
	}
	st->status = 2;                         /* Committed */
	for (id h in st->scheduled) {
		((LavHandler)h)(self);
	}
	st->status = 3;                         /* Scheduled */
	pthread_mutex_lock(&gGPULock);
	for (size_t i = 0; i < st->nops && ok; i++) {
		ok = st->ops[i].kind == OP_FILL ? runFill(&st->ops[i]) : runCopy(&st->ops[i]);
	}
	pthread_mutex_unlock(&gGPULock);
	cbFreeOps(st);
	st->status = ok ? 4 : 5;                /* Completed or Error */
	for (id h in st->handlers) {
		((LavHandler)h)(self);
	}
}

static void
cbEnqueue(__unused id self, __unused SEL _cmd)
{
}

static void
cbWait(__unused id self, __unused SEL _cmd)
{
	/* commit runs everything before it returns */
}

static void
cbAddHandler(id self, __unused SEL _cmd, id block)
{
	[STATE(self, struct cbstate)->handlers addObject:[[block copy] autorelease]];
}

static void
cbAddScheduled(id self, __unused SEL _cmd, id block)
{
	[STATE(self, struct cbstate)->scheduled addObject:[[block copy] autorelease]];
}

static uint64_t
cbStatus(id self, __unused SEL _cmd)
{
	return (uint64_t)STATE(self, struct cbstate)->status;
}

static id
cbError(id self, __unused SEL _cmd)
{
	if (STATE(self, struct cbstate)->status != 5) {
		return nil;
	}
	return [NSError errorWithDomain:@"MTLCommandBufferErrorDomain" code:1
	    userInfo:@{ NSLocalizedDescriptionKey: @"LavenderMetal: the GPU did not complete the work" }];
}

static id
cbQueue(id self, __unused SEL _cmd)
{
	return STATE(self, struct cbstate)->queue;
}

static id
cbLabel(id self, __unused SEL _cmd)
{
	return STATE(self, struct cbstate)->label;
}

static void
cbSetLabel(id self, __unused SEL _cmd, id label)
{
	struct cbstate * st = STATE(self, struct cbstate);

	[st->label release];
	st->label = [label copy];
}

static double
cbNoTime(__unused id self, __unused SEL _cmd)
{
	return 0;
}

/* ---- the device ---- */

static id
deviceInit(id self, __unused SEL _cmd, unsigned int port)
{
	struct objc_super sup = { self, gDeviceBase };
	id (*send)(struct objc_super *, SEL) = (id (*)(struct objc_super *, SEL))objc_msgSendSuper;

	self = send(&sup, sel_registerName("init"));
	gPort = port;
	gDevice = self;
	lavlog("a device for the accelerator port %u (%s)", port, self != nil ? "made" : "_MTLDevice's init gave nil");
	return self;
}

static id
deviceName(__unused id self, __unused SEL _cmd)
{
	return @"Lavender Adreno 512";
}

static uint64_t
deviceRegistryID(__unused id self, __unused SEL _cmd)
{
	return gPort;
}

static uint64_t
deviceMaxBufferLength(__unused id self, __unused SEL _cmd)
{
	return 16ull << 20;
}

static BOOL
deviceYes(__unused id self, __unused SEL _cmd)
{
	return YES;
}

static BOOL
deviceNo(__unused id self, __unused SEL _cmd)
{
	return NO;
}

static BOOL
deviceNoFeatureSet(__unused id self, __unused SEL _cmd, __unused uint64_t set)
{
	return NO;
}

static uint64_t
deviceZero(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static Class
newClass(const char * name, const char * base)
{
	Class b = objc_getClass(base);
	Class cls;

	if (b == Nil) {
		lavlog("this system has no %s: %s is made on NSObject", base, name);
		b = objc_getClass("NSObject");
	}
	cls = objc_allocateClassPair(b, name, 0);
	if (cls == Nil) {
		lavlog("could not make the class %s", name);
	}
	return cls;
}

#define ADD(cls, sel, imp, types) class_addMethod(cls, sel_registerName(sel), (IMP)imp, types)

static void
registerObjectClasses(Class device)
{
	static const char * const buffer[] = { "MTLBuffer", "MTLResource", NULL };
	static const char * const cb[] = { "MTLCommandBuffer", NULL };
	static const char * const blit[] = { "MTLBlitCommandEncoder", "MTLCommandEncoder", NULL };

	if ((gBufferClass = newClass("LavenderMetalBuffer", "_MTLResource")) != Nil) {
		declareNamed(gBufferClass, buffer);
		addStubForwarding(gBufferClass);
		ADD(gBufferClass, "dealloc", bufferDealloc, "v@:");
		ADD(gBufferClass, "device", objDevice, "@@:");
		ADD(gBufferClass, "label", bufferLabel, "@@:");
		ADD(gBufferClass, "setLabel:", bufferSetLabel, "v@:@");
		ADD(gBufferClass, "contents", bufferContents, "^v@:");
		ADD(gBufferClass, "length", bufferLength, "Q@:");
		ADD(gBufferClass, "resourceOptions", bufferOptions, "Q@:");
		ADD(gBufferClass, "storageMode", bufferStorageMode, "Q@:");
		ADD(gBufferClass, "cpuCacheMode", bufferCacheMode, "Q@:");
		ADD(gBufferClass, "hazardTrackingMode", bufferHazardMode, "Q@:");
		ADD(gBufferClass, "allocatedSize", bufferLength, "Q@:");
		ADD(gBufferClass, "gpuAddress", bufferGPUAddress, "Q@:");
		ADD(gBufferClass, "didModifyRange:", bufferDidModify, "v@:{_NSRange=QQ}");
		ADD(gBufferClass, "setPurgeableState:", bufferPurgeable, "Q@:Q");
		objc_registerClassPair(gBufferClass);
		ADD(device, "newBufferWithLength:options:", deviceNewBuffer, "@@:QQ");
		ADD(device, "newBufferWithBytes:length:options:", deviceNewBufferBytes, "@@:r^vQQ");
	}
	if ((gQueueClass = newClass("LavenderMetalQueue", "_MTLCommandQueue")) != Nil) {
		declareFamily(gQueueClass, "MTLCommandQueue");
		addStubForwarding(gQueueClass);
		ADD(gQueueClass, "device", objDevice, "@@:");
		ADD(gQueueClass, "label", queueLabel, "@@:");
		ADD(gQueueClass, "commandBuffer", queueCommandBuffer, "@@:");
		ADD(gQueueClass, "commandBufferWithUnretainedReferences", queueCommandBuffer, "@@:");
		objc_registerClassPair(gQueueClass);
		ADD(device, "newCommandQueue", deviceNewQueue, "@@:");
		ADD(device, "newCommandQueueWithMaxCommandBufferCount:", deviceNewQueueMax, "@@:Q");
	}
	if ((gCommandBufferClass = newClass("LavenderMetalCommandBuffer", "_MTLCommandBuffer")) != Nil) {
		declareNamed(gCommandBufferClass, cb);
		addStubForwarding(gCommandBufferClass);
		ADD(gCommandBufferClass, "dealloc", cbDealloc, "v@:");
		ADD(gCommandBufferClass, "device", objDevice, "@@:");
		ADD(gCommandBufferClass, "label", cbLabel, "@@:");
		ADD(gCommandBufferClass, "setLabel:", cbSetLabel, "v@:@");
		ADD(gCommandBufferClass, "commandQueue", cbQueue, "@@:");
		ADD(gCommandBufferClass, "commit", cbCommit, "v@:");
		ADD(gCommandBufferClass, "enqueue", cbEnqueue, "v@:");
		ADD(gCommandBufferClass, "waitUntilCompleted", cbWait, "v@:");
		ADD(gCommandBufferClass, "waitUntilScheduled", cbWait, "v@:");
		ADD(gCommandBufferClass, "addCompletedHandler:", cbAddHandler, "v@:@?");
		ADD(gCommandBufferClass, "addScheduledHandler:", cbAddScheduled, "v@:@?");
		ADD(gCommandBufferClass, "status", cbStatus, "Q@:");
		ADD(gCommandBufferClass, "error", cbError, "@@:");
		ADD(gCommandBufferClass, "blitCommandEncoder", cbBlitEncoder, "@@:");
		ADD(gCommandBufferClass, "GPUStartTime", cbNoTime, "d@:");
		ADD(gCommandBufferClass, "GPUEndTime", cbNoTime, "d@:");
		ADD(gCommandBufferClass, "kernelStartTime", cbNoTime, "d@:");
		ADD(gCommandBufferClass, "kernelEndTime", cbNoTime, "d@:");
		objc_registerClassPair(gCommandBufferClass);
	}
	if ((gBlitClass = newClass("LavenderMetalBlitEncoder", "_MTLCommandEncoder")) != Nil) {
		declareNamed(gBlitClass, blit);
		addStubForwarding(gBlitClass);
		ADD(gBlitClass, "dealloc", encDealloc, "v@:");
		ADD(gBlitClass, "device", objDevice, "@@:");
		ADD(gBlitClass, "label", encLabel, "@@:");
		ADD(gBlitClass, "setLabel:", encSetLabel, "v@:@");
		ADD(gBlitClass, "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", encCopy, "v@:@Q@QQ");
		ADD(gBlitClass, "fillBuffer:range:value:", encFill, "v@:@{_NSRange=QQ}C");
		ADD(gBlitClass, "endEncoding", encEnd, "v@:");
		ADD(gBlitClass, "synchronizeResource:", encNothing, "v@:@");
		objc_registerClassPair(gBlitClass);
	}
}

__attribute__((constructor)) static void
lavenderMetalLoad(void)
{
	Class cls;

	gDeviceBase = objc_getClass("_MTLDevice");
	if (gDeviceBase == Nil) {
		lavlog("the system has no _MTLDevice; this plug-in registers nothing");
		return;
	}
	cls = objc_allocateClassPair(gDeviceBase, "LavenderMetalDevice", 0);
	if (cls == Nil) {
		lavlog("LavenderMetalDevice could not be made (already there?)");
		return;
	}
	declareFamily(cls, "MTLDevice");
	addStubForwarding(cls);
	ADD(cls, "initWithAcceleratorPort:", deviceInit, "@@:I");
	ADD(cls, "name", deviceName, "@@:");
	ADD(cls, "registryID", deviceRegistryID, "Q@:");
	ADD(cls, "maxBufferLength", deviceMaxBufferLength, "Q@:");
	ADD(cls, "hasUnifiedMemory", deviceYes, "B@:");
	ADD(cls, "isLowPower", deviceNo, "B@:");
	ADD(cls, "isRemovable", deviceNo, "B@:");
	ADD(cls, "isHeadless", deviceNo, "B@:");
	ADD(cls, "supportsFeatureSet:", deviceNoFeatureSet, "B@:Q");
	ADD(cls, "dedicatedMemorySize", deviceZero, "Q@:");
	registerObjectClasses(cls);
	objc_registerClassPair(cls);
	lavlog("loaded: LavenderMetalDevice and its objects are registered (base %s)", class_getName(gDeviceBase));
}
