/*
 * lavmetal_test: asks the LavenderGPU service to name the Metal plug-in on IOAcceleratorES, then uses Metal the way a program
 * does: the device, buffers, a command queue and command buffers with a blit encoder (fill and copy), and checks every byte.
 * The plug-in's own log is /var/root/lavmetal.log.
 *
 *   lavmetal_test        the whole check (M5.1 + M5.2), stage by stage, stopping at the first failure
 *   lavmetal_test off    only take the plug-in properties away (a crashed run leaves them set)
 *
 * Build (on the Mac): userland/build-lavmetal.sh
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { kLavMetal = 5 };

static io_connect_t gConn;

static kern_return_t
metalProps(io_connect_t conn, uint32_t on)
{
	return IOConnectCallStructMethod(conn, kLavMetal, &on, sizeof(on), NULL, NULL);
}

/* Metal aborts the process when a plug-in is not what it wants: take the properties away first */
static void
crashed(int sig)
{
	uint32_t off = 0;

	if (gConn != 0) {
		IOConnectCallStructMethod(gConn, kLavMetal, &off, sizeof(off), NULL, NULL);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static int gFailed;

static void
check(BOOL ok, const char * what)
{
	printf("  %-62s %s\n", what, ok ? "OK" : "WRONG");
	if (!ok) {
		gFailed++;
	}
}

/* every byte of p[off, off+len) equals f(i) */
static BOOL
rangeIs(const uint8_t * p, size_t off, size_t len, uint8_t (^f)(size_t))
{
	for (size_t i = off; i < off + len; i++) {
		if (p[i] != f(i)) {
			printf("    first difference at byte %zu: %02x, wanted %02x\n", i, p[i], f(i));
			return NO;
		}
	}
	return YES;
}

static BOOL
run(id<MTLCommandQueue> queue, void (^encode)(id<MTLBlitCommandEncoder>))
{
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	__block BOOL handled = NO;

	if (cb == nil) {
		printf("  commandBuffer: nil\n");
		return NO;
	}
	[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
		handled = YES;
		(void)done;
	}];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	if (blit == nil) {
		printf("  blitCommandEncoder: nil\n");
		return NO;
	}
	encode(blit);
	[blit endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	printf("  command buffer status %lu, completed handler %s\n", (unsigned long)[cb status], handled ? "called" : "NOT called");
	return [cb status] == MTLCommandBufferStatusCompleted && handled;
}

static uint8_t zero(size_t i) { (void)i; return 0; }

int
main(int argc, char ** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	@autoreleasepool {
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
		io_connect_t conn = 0;
		kern_return_t kr;

		if (service == 0) {
			printf("no LavenderGPU service\n");
			return 1;
		}
		kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
		printf("IOServiceOpen: kr %#x\n", kr);
		if (kr != KERN_SUCCESS) {
			return 1;
		}
		gConn = conn;
		signal(SIGABRT, crashed);
		signal(SIGSEGV, crashed);
		signal(SIGTRAP, crashed);
		signal(SIGILL, crashed);
		if (argc > 1 && strcmp(argv[1], "off") == 0) {
			kr = metalProps(conn, 0);
			printf("properties off: kr %#x\n", kr);
			return kr == KERN_SUCCESS ? 0 : 1;
		}

		kr = metalProps(conn, 1);
		printf("properties on: kr %#x\n", kr);
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		printf("MTLCreateSystemDefaultDevice: %s\n", device != nil ? "a device" : "nil");
		if (device == nil) {
			metalProps(conn, 0);
			printf("FAILED\n");
			return 1;
		}
		printf("  class %s, name %s, maxBufferLength %llu\n", object_getClassName(device), [[device name] UTF8String],
		    (unsigned long long)[device maxBufferLength]);

		id<MTLCommandQueue> queue = [device newCommandQueue];
		id<MTLBuffer> A = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
		id<MTLBuffer> B = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
		printf("newCommandQueue: %s, buffers: %s %s\n", queue ? "an object" : "nil", A ? "A" : "nil", B ? "B" : "nil");
		if (queue == nil || A == nil || B == nil) {
			metalProps(conn, 0);
			printf("FAILED\n");
			return 1;
		}
		printf("  A: length %lu\n", (unsigned long)[A length]);
		uint8_t * a = [A contents];
		uint8_t * b = [B contents];

		printf("stage 1: the CPU's view of a buffer\n");
		check(rangeIs(b, 0, 65536, ^uint8_t(size_t i) { return zero(i); }), "a new buffer reads zero");
		for (size_t i = 0; i < 65536; i++) {
			a[i] = (uint8_t)(i * 7 + 1);
		}

		if (gFailed == 0) {
			printf("stage 2: fill by the GPU (CP_MEM_WRITE), 4096 aligned bytes\n");
			BOOL ok = run(queue, ^(id<MTLBlitCommandEncoder> e) {
				[e fillBuffer:B range:NSMakeRange(0, 4096) value:0xab];
			});
			check(ok, "the command buffer completed");
			check(rangeIs(b, 0, 4096, ^uint8_t(size_t i) { (void)i; return 0xab; }), "bytes 0..4095 are 0xab");
			check(rangeIs(b, 4096, 100, ^uint8_t(size_t i) { return zero(i); }), "bytes after the range stay zero");
		}
		if (gFailed == 0) {
			printf("stage 3: fill with an unaligned start and length\n");
			BOOL ok = run(queue, ^(id<MTLBlitCommandEncoder> e) {
				[e fillBuffer:B range:NSMakeRange(8195, 1001) value:0x5a];
			});
			check(ok, "the command buffer completed");
			check(rangeIs(b, 8195, 1001, ^uint8_t(size_t i) { (void)i; return 0x5a; }), "bytes 8195..9195 are 0x5a");
			check(rangeIs(b, 8192, 3, ^uint8_t(size_t i) { return zero(i); }) &&
			    rangeIs(b, 9196, 20, ^uint8_t(size_t i) { return zero(i); }), "the bytes either side stay zero");
		}
		if (gFailed == 0) {
			printf("stage 4: copy 64 bytes by the 2D core (A[0..64) -> B[16384..16448))\n");
			BOOL ok = run(queue, ^(id<MTLBlitCommandEncoder> e) {
				[e copyFromBuffer:A sourceOffset:0 toBuffer:B destinationOffset:16384 size:64];
			});
			check(ok, "the command buffer completed");
			check(rangeIs(b, 16384, 64, ^uint8_t(size_t i) { return (uint8_t)((i - 16384) * 7 + 1); }), "the 64 bytes are A's");
			check(rangeIs(b, 16448, 64, ^uint8_t(size_t i) { return zero(i); }), "the bytes after stay zero");
		}
		if (gFailed == 0) {
			printf("stage 5: copy 8192 bytes (A[0..8192) -> B[32768..40960))\n");
			BOOL ok = run(queue, ^(id<MTLBlitCommandEncoder> e) {
				[e copyFromBuffer:A sourceOffset:0 toBuffer:B destinationOffset:32768 size:8192];
			});
			check(ok, "the command buffer completed");
			check(rangeIs(b, 32768, 8192, ^uint8_t(size_t i) { return (uint8_t)((i - 32768) * 7 + 1); }), "the 8192 bytes are A's");
			check(rangeIs(b, 40960, 100, ^uint8_t(size_t i) { return zero(i); }), "the bytes after stay zero");
		}
		if (gFailed == 0) {
			printf("stage 6: copy with an unaligned start and length (A[5..20005) -> B[41005..61005))\n");
			BOOL ok = run(queue, ^(id<MTLBlitCommandEncoder> e) {
				[e copyFromBuffer:A sourceOffset:5 toBuffer:B destinationOffset:41005 size:20000];
			});
			check(ok, "the command buffer completed");
			check(rangeIs(b, 41005, 20000, ^uint8_t(size_t i) { return (uint8_t)((i - 41005 + 5) * 7 + 1); }), "the 20000 bytes are A's");
			check(rangeIs(b, 41000, 5, ^uint8_t(size_t i) { return zero(i); }), "the bytes before stay as they were");
		}
		if (gFailed == 0) {
			printf("stage 6b: copy whose ends are unaligned but alike (A[5..20005) -> B[41029..61029)): the CPU does the edges, the 2D core the middle\n");
			BOOL ok = run(queue, ^(id<MTLBlitCommandEncoder> e) {
				[e copyFromBuffer:A sourceOffset:5 toBuffer:B destinationOffset:41029 size:20000];
			});
			check(ok, "the command buffer completed");
			check(rangeIs(b, 41029, 20000, ^uint8_t(size_t i) { return (uint8_t)((i - 41029 + 5) * 7 + 1); }), "the 20000 bytes are A's");
			check(rangeIs(b, 61029, 8, ^uint8_t(size_t i) { return zero(i); }), "the bytes after stay zero");
		}
		if (gFailed == 0) {
			printf("stage 7: two operations in one command buffer, in order\n");
			BOOL ok = run(queue, ^(id<MTLBlitCommandEncoder> e) {
				[e fillBuffer:B range:NSMakeRange(61440, 1024) value:0x33];
				[e copyFromBuffer:B sourceOffset:61440 toBuffer:B destinationOffset:62464 size:1024];
			});
			check(ok, "the command buffer completed");
			check(rangeIs(b, 62464, 1024, ^uint8_t(size_t i) { (void)i; return 0x33; }), "the copy sees the fill: 1024 bytes of 0x33");
		}

		kr = metalProps(conn, 0);
		printf("properties off: kr %#x\n", kr);
		IOServiceClose(conn);
		printf("%s\n", gFailed == 0 ? "PASSED" : "FAILED");
		return gFailed == 0 ? 0 : 1;
	}
}
