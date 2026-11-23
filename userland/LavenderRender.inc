/* LavenderRender.inc: M5.4 - textures, render pipeline states and the render encoder. Included by LavenderMetal.m (one
 * translation unit), after the compute classes and before the device. One draw is one stream (lava5r.c), sysmem mode; the
 * colour target is a texture the encoder cleared (by the CPU) or loaded. No depth, blending or sampling yet; what is not
 * served is refused by name in the log. */

typedef struct { double x, y, w, h, znear, zfar; } LavViewport;
typedef struct { uint64_t x, y, w, h; } LavScissor;
typedef struct { uint64_t x, y, z; } LavOrigin;
typedef struct { LavOrigin origin; LavSize size; } LavRegion;
typedef struct { double r, g, b, a; } LavClear;

static id
msg0(id o, const char * sel)
{
	return o != nil ? ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel)) : nil;
}

static uint64_t
msgU(id o, const char * sel)
{
	return o != nil ? ((uint64_t (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel)) : 0;
}

static id
msgAt(id o, const char * sel, uint64_t i)
{
	return o != nil ? ((id (*)(id, SEL, uint64_t))objc_msgSend)(o, sel_registerName(sel), i) : nil;
}

#define PF_RGBA8 70
#define PF_BGRA8 80

struct texstate {
	IOSurfaceRef surface;                   /* retained: the texture is a CAMetalLayer drawable; present copies into it */
	struct lbuf b;
	uint32_t width, height, format, pitch;
	uint64_t usage, storage;
	id label;
};

static id
deviceNewTexture(__unused id self, __unused SEL _cmd, id desc)
{
	uint64_t w = msgU(desc, "width"), h = msgU(desc, "height"), fmt = msgU(desc, "pixelFormat"), type = msgU(desc, "textureType");
	uint64_t depth = msgU(desc, "depth"), mips = msgU(desc, "mipmapLevelCount"), samples = msgU(desc, "sampleCount");
	uint64_t layers = msgU(desc, "arrayLength");
	struct texstate * st;
	id o;
	size_t pitch, len;
	BOOL ok;

	if (!lavOpen() || w == 0 || h == 0 || w > 4096 || h > 4096) {
		lavlog("newTexture %llux%llu refused", (unsigned long long)w, (unsigned long long)h);
		return nil;
	}
	if ((fmt != PF_RGBA8 && fmt != PF_BGRA8) || type != 2 || depth > 1 || mips > 1 || samples > 1 || layers > 1) {
		lavlog("newTexture: pixel format %llu, type %llu, %llu mips, %llu samples, %llu layers: not served (2D RGBA8/BGRA8 only)",
		    (unsigned long long)fmt, (unsigned long long)type, (unsigned long long)mips, (unsigned long long)samples,
		    (unsigned long long)layers);
		return nil;
	}
	pitch = ((size_t)w * 4 + 63) & ~(size_t)63;
	len = pitch * (size_t)h;
	if (len > (16u << 20)) {
		return nil;
	}
	o = makeObject(gTextureClass, sizeof(struct texstate));
	if (o == nil) {
		return nil;
	}
	st = STATE(o, struct texstate);
	memset(st, 0, sizeof(*st));
	pthread_mutex_lock(&gGPULock);
	ok = lavBufNew((len + 4095) & ~(size_t)4095, &st->b);
	pthread_mutex_unlock(&gGPULock);
	if (!ok) {
		return nil;
	}
	memset(st->b.cpu, 0, st->b.len);
	st->width = (uint32_t)w;
	st->height = (uint32_t)h;
	st->format = (uint32_t)fmt;
	st->pitch = (uint32_t)pitch;
	st->usage = msgU(desc, "usage");
	st->storage = msgU(desc, "storageMode");
	lavlog("texture %llux%llu format %llu, pitch %zu, at GPU %#llx", (unsigned long long)w, (unsigned long long)h,
	    (unsigned long long)fmt, pitch, (unsigned long long)st->b.gpu);
	return o;
}

/* IOSurface <-> the texture's own memory (the GPU cannot reach an IOSurface's scattered pages): rows copied at the edges */
static void
surfaceCopy(struct texstate * t, BOOL toSurface)
{
	size_t sp, rows = t->height, row = (size_t)t->width * 4;
	uint8_t * base;

	if (IOSurfaceLock(t->surface, toSurface ? 0 : kIOSurfaceLockReadOnly, NULL) != 0) {
		lavlog("IOSurfaceLock failed");
		return;
	}
	sp = IOSurfaceGetBytesPerRow(t->surface);
	base = IOSurfaceGetBaseAddress(t->surface);
	if (IOSurfaceGetHeight(t->surface) < rows) {
		rows = IOSurfaceGetHeight(t->surface);
	}
	for (size_t y = 0; y < rows; y++) {
		if (toSurface) {
			memcpy(base + y * sp, (uint8_t *)t->b.cpu + y * t->pitch, row);
		} else {
			memcpy((uint8_t *)t->b.cpu + y * t->pitch, base + y * sp, row);
		}
	}
	IOSurfaceUnlock(t->surface, toSurface ? 0 : kIOSurfaceLockReadOnly, NULL);
}

static void
cbSyncSurfaces(struct cbstate * st)
{
	for (size_t i = 0; i < st->nops; i++) {
		id tex = st->ops[i].kind == OP_DRAW ? st->ops[i].draw->target : st->ops[i].kind == OP_CLEAR ? st->ops[i].dst : nil;

		if (tex != nil && STATE(tex, struct texstate)->surface != NULL) {
			surfaceCopy(STATE(tex, struct texstate), YES);
		}
	}
}

static id
deviceNewTextureSurface(id self, SEL cmd, id desc, IOSurfaceRef surface, uint64_t plane)
{
	id o;
	struct texstate * st;

	if (surface == NULL || plane != 0 || IOSurfaceGetPixelFormat(surface) != 'BGRA') {
		lavlog("newTexture from an IOSurface: plane %llu, pixel format %#x: only plane 0 of BGRA is served",
		    (unsigned long long)plane, surface != NULL ? IOSurfaceGetPixelFormat(surface) : 0);
		return nil;
	}
	o = deviceNewTexture(self, cmd, desc);
	if (o == nil) {
		return nil;
	}
	st = STATE(o, struct texstate);
	if (st->format != PF_BGRA8 || IOSurfaceGetWidth(surface) < st->width || IOSurfaceGetHeight(surface) < st->height) {
		lavlog("newTexture from an IOSurface: the surface (%zux%zu) does not hold the texture (%ux%u)", IOSurfaceGetWidth(surface),
		    IOSurfaceGetHeight(surface), st->width, st->height);
		[o release];
		return nil;
	}
	st->surface = (IOSurfaceRef)CFRetain(surface);
	lavlog("texture over an IOSurface %zux%zu, row %zu bytes", IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
	    IOSurfaceGetBytesPerRow(surface));
	return o;
}

static void
cbPresent(id self, __unused SEL _cmd, id drawable)
{
	struct cbstate * st = STATE(self, struct cbstate);

	if (st->drawables == nil) {
		st->drawables = [NSMutableArray new];
	}
	[st->drawables addObject:drawable];
}

static void
cbPresentAt(id self, SEL cmd, id drawable, __unused double t)
{
	cbPresent(self, cmd, drawable);
}

static void
texDealloc(id self, __unused SEL _cmd)
{
	struct texstate * st = STATE(self, struct texstate);
	struct objc_super sup = { self, class_getSuperclass(gTextureClass) };

	pthread_mutex_lock(&gGPULock);
	lavBufFree(&st->b);
	pthread_mutex_unlock(&gGPULock);
	if (st->surface != NULL) {
		CFRelease(st->surface);
	}
	[st->label release];
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static IOSurfaceRef texIOSurface(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->surface; }
static uint64_t texWidth(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->width; }
static uint64_t texHeight(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->height; }
static uint64_t texOne(__unused id self, __unused SEL _cmd) { return 1; }
static uint64_t texType(__unused id self, __unused SEL _cmd) { return 2; }
static uint64_t texFormat(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->format; }
static uint64_t texUsage(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->usage; }
static uint64_t texStorage(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->storage; }
static uint64_t texSize(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->b.len; }
static id texLabel(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->label; }

static void
texSetLabel(id self, __unused SEL _cmd, id label)
{
	struct texstate * st = STATE(self, struct texstate);

	[st->label release];
	st->label = [label copy];
}

static BOOL
texRegionOk(struct texstate * st, LavRegion r, uint64_t level)
{
	return level == 0 && r.origin.x + r.size.width <= st->width && r.origin.y + r.size.height <= st->height;
}

static void
texGetBytes(id self, __unused SEL _cmd, void * dst, uint64_t pitch, LavRegion r, uint64_t level)
{
	struct texstate * st = STATE(self, struct texstate);

	if (!texRegionOk(st, r, level)) {
		lavlog("getBytes: region outside the texture");
		return;
	}
	for (uint64_t y = 0; y < r.size.height; y++) {
		memcpy((uint8_t *)dst + y * pitch, (uint8_t *)st->b.cpu + (r.origin.y + y) * st->pitch + r.origin.x * 4, r.size.width * 4);
	}
}

static void
texGetBytesSlice(id self, SEL cmd, void * dst, uint64_t pitch, __unused uint64_t image, LavRegion r, uint64_t level, __unused uint64_t slice)
{
	texGetBytes(self, cmd, dst, pitch, r, level);
}

static void
texReplace(id self, __unused SEL _cmd, LavRegion r, uint64_t level, const void * src, uint64_t pitch)
{
	struct texstate * st = STATE(self, struct texstate);

	if (!texRegionOk(st, r, level)) {
		lavlog("replaceRegion: region outside the texture");
		return;
	}
	for (uint64_t y = 0; y < r.size.height; y++) {
		memcpy((uint8_t *)st->b.cpu + (r.origin.y + y) * st->pitch + r.origin.x * 4, (const uint8_t *)src + y * pitch, r.size.width * 4);
	}
}

static void
texReplaceSlice(id self, SEL cmd, LavRegion r, uint64_t level, __unused uint64_t slice, const void * src, uint64_t pitch, __unused uint64_t image)
{
	texReplace(self, cmd, r, level, src, pitch);
}

/* ---- the render pipeline state ---- */

struct rpipe {
	struct lav5_blend blend[8];
	id vfn, ffn;                            /* retained */
	struct lav5_stage vs, fs;
	struct lav5_link link;
	struct lbuf vcode, fcode;
	uint32_t nattr;
	struct lav5_attr attr[16];
	uint32_t stride[LAV5_MAX_BUFFERS];
	id label;
};

static BOOL
codeBuf(struct lbuf * b, const struct lav5_kernel * k)
{
	size_t len = (size_t)k->instrlen * 128;
	BOOL ok;

	if (len < (size_t)k->nwords * 4) {
		len = (size_t)k->nwords * 4;
	}
	pthread_mutex_lock(&gGPULock);
	ok = lavBufNew((len + 4095) & ~(size_t)4095, b);
	pthread_mutex_unlock(&gGPULock);
	if (ok) {
		memset(b->cpu, 0, b->len);
		memcpy(b->cpu, k->words, (size_t)k->nwords * 4);
	}
	return ok;
}

/* Metal's MTLVertexFormat to an a5xx one: float, float2, float3, float4, uchar4Normalized */
static BOOL
vertexFormat(uint64_t f, uint32_t * fmt, uint32_t * isfloat)
{
	*isfloat = 1;
	switch (f) {
	case 28: *fmt = 74; return YES;
	case 29: *fmt = 103; return YES;
	case 30: *fmt = 116; return YES;
	case 31: *fmt = 130; return YES;
	case 9: *fmt = 48; return YES;
	}
	return NO;
}

static BOOL
readBlend(id desc, uint64_t index, struct lav5_blend * out)
{
	id ca = msgAt(msg0(desc, "colorAttachments"), "objectAtIndexedSubscript:", index);
	static const uint8_t factor[15] = { 0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 16, 12, 13, 14, 15 };
	uint64_t wm = msgU(ca, "writeMask");

	memset(out, 0, sizeof(*out));
	out->writemask = ((wm >> 3) & 1) | (((wm >> 2) & 1) << 1) | (((wm >> 1) & 1) << 2) | ((wm & 1) << 3);
	out->enable = msgU(ca, "isBlendingEnabled") & 0xff ? 1 : 0;
	if (out->enable) {
		uint64_t f[4] = { msgU(ca, "sourceRGBBlendFactor"), msgU(ca, "destinationRGBBlendFactor"),
		    msgU(ca, "sourceAlphaBlendFactor"), msgU(ca, "destinationAlphaBlendFactor") };
		uint64_t ops[2] = { msgU(ca, "rgbBlendOperation"), msgU(ca, "alphaBlendOperation") };

		for (int i = 0; i < 4; i++) {
			if (f[i] >= 15) {
				lavlog("render pipeline: blend factor %llu is not served", (unsigned long long)f[i]);
				return NO;
			}
		}
		for (int i = 0; i < 2; i++) {
			if (ops[i] > 4) {
				lavlog("render pipeline: blend operation %llu is not served", (unsigned long long)ops[i]);
				return NO;
			}
		}
		out->rgb_src = factor[f[0]];
		out->rgb_dst = factor[f[1]];
		out->a_src = factor[f[2]];
		out->a_dst = factor[f[3]];
		out->rgb_op = (uint32_t)ops[0];
		out->a_op = (uint32_t)ops[1];
	}
	return YES;
}

static id
makeRenderPipeline(id desc, NSError ** err)
{
	id vf = msg0(desc, "vertexFunction"), ff = msg0(desc, "fragmentFunction");
	id vd = msg0(desc, "vertexDescriptor");
	struct fnstate * vs, * fs;
	struct rpipe * rp;
	char why[256] = "";
	id o;
	int ok;

	if (vf == nil || ff == nil || !lavOpen()) {
		lavError(err, @"MTLLibraryErrorDomain", "a render pipeline needs a vertex and a fragment function");
		return nil;
	}
	vs = STATE(vf, struct fnstate);
	fs = STATE(ff, struct fnstate);
	if (vs->type != 0 || fs->type != 1) {
		lavError(err, @"MTLLibraryErrorDomain", "wrong kinds of functions");
		return nil;
	}
	o = makeObject(gRenderPipeClass, sizeof(struct rpipe));
	if (o == nil) {
		return nil;
	}
	rp = STATE(o, struct rpipe);
	memset(rp, 0, sizeof(*rp));
	pthread_mutex_lock(&gCCLock);
	ok = lavcc_compile_program(STATE(vs->library, struct libstate)->lib, [vs->name UTF8String],
	    STATE(fs->library, struct libstate)->lib, [fs->name UTF8String], &rp->vs, &rp->fs, &rp->link, why, sizeof(why));
	pthread_mutex_unlock(&gCCLock);
	if (!ok) {
		lavlog("render pipeline %s + %s: the compiler refused it: %s", [vs->name UTF8String], [fs->name UTF8String], why);
		lavError(err, @"MTLLibraryErrorDomain", why);
		[o release];
		return nil;
	}
	/* the attributes the vertex function reads, from the vertex descriptor */
	for (uint32_t j = 0; j < rp->vs.nin; j++) {
		const struct lav5_io * in = &rp->vs.in[j];
		id a, l;
		uint32_t fmt, isfloat, buffer;

		if (in->sysval || in->compmask == 0 || in->slot < 15) {
			continue;
		}
		if (rp->nattr >= 16 || vd == nil) {
			lavlog("render pipeline: attribute %u is read but the vertex descriptor has none", in->slot - 15);
			goto refuse;
		}
		a = msgAt(msg0(vd, "attributes"), "objectAtIndexedSubscript:", in->slot - 15);
		buffer = (uint32_t)msgU(a, "bufferIndex");
		if (!vertexFormat(msgU(a, "format"), &fmt, &isfloat) || buffer >= LAV5_MAX_BUFFERS) {
			lavlog("render pipeline: attribute %u has vertex format %llu (not served) or buffer %u", in->slot - 15,
			    (unsigned long long)msgU(a, "format"), buffer);
			goto refuse;
		}
		rp->attr[rp->nattr++] = (struct lav5_attr){ in->slot - 15, fmt, (uint32_t)msgU(a, "offset"), buffer, isfloat, 0 };
		l = msgAt(msg0(vd, "layouts"), "objectAtIndexedSubscript:", buffer);
		if (msgU(l, "stepFunction") != 1) {
			lavlog("render pipeline: buffer %u steps per instance or constant: not served", buffer);
			goto refuse;
		}
		rp->stride[buffer] = (uint32_t)msgU(l, "stride");
	}
	if (rp->nattr != rp->vs.nattr) {
		lavlog("render pipeline: the vertex function has %u attributes, the descriptor gave %u", rp->vs.nattr, rp->nattr);
		goto refuse;
	}
	if (!codeBuf(&rp->vcode, &rp->vs.k) || !codeBuf(&rp->fcode, &rp->fs.k)) {
		lavError(err, @"MTLLibraryErrorDomain", "no memory for the code");
		[o release];
		return nil;
	}
	/* blending and the write mask, from each colour attachment */
	for (uint64_t ai = 0; ai < 8; ai++) {
		if (!readBlend(desc, ai, &rp->blend[ai])) {
			goto refuse;
		}
	}
	rp->vfn = [vf retain];
	rp->ffn = [ff retain];
	lavlog("render pipeline %s + %s: %u+%u words, %u attributes", [vs->name UTF8String], [fs->name UTF8String], rp->vs.k.nwords,
	    rp->fs.k.nwords, rp->nattr);
	return o;
refuse:
	lavError(err, @"MTLLibraryErrorDomain", "the render pipeline uses something this driver does not serve (see the log)");
	[o release];
	return nil;
}

static id
deviceNewRenderPipeline(__unused id self, __unused SEL _cmd, id desc, NSError ** err)
{
	return makeRenderPipeline(desc, err);
}

static id
deviceNewRenderPipelineOptions(__unused id self, __unused SEL _cmd, id desc, __unused uint64_t options, id * reflection, NSError ** err)
{
	if (reflection != NULL) {
		*reflection = nil;
	}
	return makeRenderPipeline(desc, err);
}

static void
deviceNewRenderPipelineAsync(__unused id self, __unused SEL _cmd, id desc, id handler)
{
	NSError * err = nil;
	id p = makeRenderPipeline(desc, &err);

	((LavPipelineDone)handler)(p, err);
	[p release];
}

static void
rpDealloc(id self, __unused SEL _cmd)
{
	struct rpipe * st = STATE(self, struct rpipe);
	struct objc_super sup = { self, class_getSuperclass(gRenderPipeClass) };

	lavlog("render pipeline freed (code handles %u, %u)", st->vcode.handle, st->fcode.handle);
	pthread_mutex_lock(&gGPULock);
	if (st->vcode.handle != 0) {
		lavBufFree(&st->vcode);
	}
	if (st->fcode.handle != 0) {
		lavBufFree(&st->fcode);
	}
	pthread_mutex_unlock(&gGPULock);
	lavcc_stage_free(&st->vs);
	lavcc_stage_free(&st->fs);
	[st->vfn release];
	[st->ffn release];
	[st->label release];
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static id rpLabel(id self, __unused SEL _cmd) { return STATE(self, struct rpipe)->label; }

static void
rpSetLabel(id self, __unused SEL _cmd, id label)
{
	struct rpipe * st = STATE(self, struct rpipe);

	[st->label release];
	st->label = [label copy];
}

/* ---- the render encoder ---- */

struct renc {
	id commandBuffer;                       /* retained */
	BOOL ended;
	id label;
	id target;                              /* retained */
	id xtarget[8];                          /* retained: colour targets 1.. */
	id pipeline;                            /* retained */
	id vbuf[LAV5_MAX_BUFFERS];
	uint64_t voff[LAV5_MAX_BUFFERS];
	uint8_t * vbytes[LAV5_MAX_BUFFERS];
	uint32_t vblen[LAV5_MAX_BUFFERS];
	id fbuf[LAV5_MAX_BUFFERS];
	uint64_t foff[LAV5_MAX_BUFFERS];
	uint8_t * fbytes[LAV5_MAX_BUFFERS];
	uint32_t fblen[LAV5_MAX_BUFFERS];
	float viewport[6];
	uint32_t scissor[4];
	uint32_t cull, front_cw;
	id ftex[16], fsamp[16];
	float blend_color[4];
};

static id
cbRenderEncoder(id self, __unused SEL _cmd, id passdesc)
{
	id atts = msg0(passdesc, "colorAttachments");
	id att = msgAt(atts, "objectAtIndexedSubscript:", 0);
	id tex = msg0(att, "texture");
	id e;
	struct renc * re;
	struct texstate * ts;

	if (tex == nil || object_getClass(tex) != gTextureClass) {
		lavlog("renderCommandEncoder: colour attachment 0 is not a texture of this driver (drawables come with M5.5)");
		return nil;
	}
	if (msg0(msg0(passdesc, "depthAttachment"), "texture") != nil || msg0(msg0(passdesc, "stencilAttachment"), "texture") != nil) {
		lavlog("renderCommandEncoder: depth and stencil attachments are not served (ignored)");
	}
	e = makeObject(gRenderClass, sizeof(struct renc));
	if (e == nil) {
		return nil;
	}
	re = STATE(e, struct renc);
	memset(re, 0, sizeof(*re));
	re->commandBuffer = [self retain];
	re->target = [tex retain];
	ts = STATE(tex, struct texstate);
	for (uint64_t i = 1; i < 8; i++) {
		id a = msgAt(atts, "objectAtIndexedSubscript:", i), t = msg0(a, "texture");

		if (t == nil) {
			continue;
		}
		if (object_getClass(t) != gTextureClass || STATE(t, struct texstate)->width != ts->width ||
		    STATE(t, struct texstate)->height != ts->height) {
			lavlog("renderCommandEncoder: colour attachment %llu is not a texture of this driver of the same size as attachment 0", (unsigned long long)i);
			continue;
		}
		re->xtarget[i] = [t retain];
		if (msgU(a, "loadAction") == 2) {
			struct op * cop = cbAddOp(STATE(self, struct cbstate));
			LavClear c = ((LavClear (*)(id, SEL))objc_msgSend)(a, sel_registerName("clearColor"));

			cop->kind = OP_CLEAR;
			cop->dst = [t retain];
			cop->clear[0] = c.r;
			cop->clear[1] = c.g;
			cop->clear[2] = c.b;
			cop->clear[3] = c.a;
		}
	}
	re->viewport[2] = (float)ts->width;
	re->viewport[3] = (float)ts->height;
	re->viewport[5] = 1;
	re->scissor[2] = ts->width;
	re->scissor[3] = ts->height;
	re->front_cw = 1;
	if (msgU(att, "loadAction") == 1 && ts->surface != NULL) {
		surfaceCopy(ts, NO);
	}
	if (msgU(att, "loadAction") == 2) {
		struct op * op = cbAddOp(STATE(self, struct cbstate));
		LavClear c = ((LavClear (*)(id, SEL))objc_msgSend)(att, sel_registerName("clearColor"));

		op->kind = OP_CLEAR;
		op->dst = [tex retain];
		op->clear[0] = c.r;
		op->clear[1] = c.g;
		op->clear[2] = c.b;
		op->clear[3] = c.a;
	}
	return [e autorelease];
}

static void
rencDealloc(id self, __unused SEL _cmd)
{
	struct renc * re = STATE(self, struct renc);
	struct objc_super sup = { self, class_getSuperclass(gRenderClass) };

	for (int i = 0; i < LAV5_MAX_BUFFERS; i++) {
		[re->vbuf[i] release];
		[re->fbuf[i] release];
		free(re->vbytes[i]);
		free(re->fbytes[i]);
	}
	for (int i = 0; i < 16; i++) {
		[re->ftex[i] release];
		[re->fsamp[i] release];
	}
	for (int i = 0; i < 8; i++) {
		[re->xtarget[i] release];
	}
	[re->pipeline release];
	[re->target release];
	[re->commandBuffer release];
	[re->label release];
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static void
rencSetPipeline(id self, __unused SEL _cmd, id pipeline)
{
	struct renc * re = STATE(self, struct renc);

	[pipeline retain];
	[re->pipeline release];
	re->pipeline = pipeline;
}

static void
rencBind(id * bufs, uint64_t * offs, uint8_t ** bytes, uint32_t * blen, id buffer, uint64_t offset, uint64_t index)
{
	if (index >= LAV5_MAX_BUFFERS) {
		lavlog("setBuffer at index %llu: the table holds %d", (unsigned long long)index, LAV5_MAX_BUFFERS);
		return;
	}
	[buffer retain];
	[bufs[index] release];
	bufs[index] = buffer;
	offs[index] = offset;
	free(bytes[index]);
	bytes[index] = NULL;
	blen[index] = 0;
}

static void
rencBindBytes(id * bufs, uint8_t ** bytes, uint32_t * blen, const void * data, uint64_t length, uint64_t index)
{
	if (index >= LAV5_MAX_BUFFERS || length == 0 || length > 4096) {
		lavlog("setBytes of %llu at index %llu refused", (unsigned long long)length, (unsigned long long)index);
		return;
	}
	[bufs[index] release];
	bufs[index] = nil;
	free(bytes[index]);
	bytes[index] = malloc((size_t)length);
	memcpy(bytes[index], data, (size_t)length);
	blen[index] = (uint32_t)length;
}

static void
rencSetVBuffer(id self, __unused SEL _cmd, id buffer, uint64_t offset, uint64_t index)
{
	struct renc * re = STATE(self, struct renc);

	rencBind(re->vbuf, re->voff, re->vbytes, re->vblen, buffer, offset, index);
}

static void
rencSetFBuffer(id self, __unused SEL _cmd, id buffer, uint64_t offset, uint64_t index)
{
	struct renc * re = STATE(self, struct renc);

	rencBind(re->fbuf, re->foff, re->fbytes, re->fblen, buffer, offset, index);
}

static void
rencSetVOffset(id self, __unused SEL _cmd, uint64_t offset, uint64_t index)
{
	if (index < LAV5_MAX_BUFFERS) {
		STATE(self, struct renc)->voff[index] = offset;
	}
}

static void
rencSetFOffset(id self, __unused SEL _cmd, uint64_t offset, uint64_t index)
{
	if (index < LAV5_MAX_BUFFERS) {
		STATE(self, struct renc)->foff[index] = offset;
	}
}

static void
rencSetVBytes(id self, __unused SEL _cmd, const void * data, uint64_t length, uint64_t index)
{
	struct renc * re = STATE(self, struct renc);

	rencBindBytes(re->vbuf, re->vbytes, re->vblen, data, length, index);
}

static void
rencSetFBytes(id self, __unused SEL _cmd, const void * data, uint64_t length, uint64_t index)
{
	struct renc * re = STATE(self, struct renc);

	rencBindBytes(re->fbuf, re->fbytes, re->fblen, data, length, index);
}

static void
rencSetViewport(id self, __unused SEL _cmd, LavViewport v)
{
	struct renc * re = STATE(self, struct renc);

	re->viewport[0] = (float)v.x;
	re->viewport[1] = (float)v.y;
	re->viewport[2] = (float)v.w;
	re->viewport[3] = (float)v.h;
	re->viewport[4] = (float)v.znear;
	re->viewport[5] = (float)v.zfar;
}

static void
rencSetScissor(id self, __unused SEL _cmd, LavScissor r)
{
	struct renc * re = STATE(self, struct renc);

	re->scissor[0] = (uint32_t)r.x;
	re->scissor[1] = (uint32_t)r.y;
	re->scissor[2] = (uint32_t)r.w;
	re->scissor[3] = (uint32_t)r.h;
}

static void
rencSetFTexture(id self, __unused SEL _cmd, id tex, uint64_t index)
{
	struct renc * re = STATE(self, struct renc);

	if (index >= 16) {
		lavlog("setFragmentTexture at index %llu: the table holds 16", (unsigned long long)index);
		return;
	}
	if (tex != nil && object_getClass(tex) != gTextureClass) {
		lavlog("setFragmentTexture: not a texture of this driver");
		return;
	}
	[tex retain];
	[re->ftex[index] release];
	re->ftex[index] = tex;
}

static void
rencSetFSampler(id self, __unused SEL _cmd, id samp, uint64_t index)
{
	struct renc * re = STATE(self, struct renc);

	if (index >= 16) {
		lavlog("setFragmentSamplerState at index %llu: the table holds 16", (unsigned long long)index);
		return;
	}
	[samp retain];
	[re->fsamp[index] release];
	re->fsamp[index] = samp;
}

static void
rencSetFSamplerLod(id self, SEL cmd, id samp, __unused float lo, __unused float hi, uint64_t index)
{
	rencSetFSampler(self, cmd, samp, index);
}

static void
rencSetFTextures(id self, SEL cmd, const id * texs, NSRange range)
{
	for (NSUInteger i = 0; i < range.length; i++) {
		rencSetFTexture(self, cmd, texs[i], range.location + i);
	}
}

static void
rencSetFSamplers(id self, SEL cmd, const id * samps, NSRange range)
{
	for (NSUInteger i = 0; i < range.length; i++) {
		rencSetFSampler(self, cmd, samps[i], range.location + i);
	}
}

static void
rencSetVTexture(__unused id self, __unused SEL _cmd, __unused id tex, __unused uint64_t index)
{
	lavlog("setVertexTexture: sampling in the vertex stage is not served");
}

static void
rencSetBlendColor(id self, __unused SEL _cmd, float r, float g, float b, float a)
{
	struct renc * re = STATE(self, struct renc);

	re->blend_color[0] = r;
	re->blend_color[1] = g;
	re->blend_color[2] = b;
	re->blend_color[3] = a;
}

/* ---- samplers ---- */

static id
deviceNewSampler(__unused id self, __unused SEL _cmd, id desc)
{
	id o = makeObject(gSamplerClass, sizeof(struct lav5_samp));
	struct lav5_samp * sp;
	uint64_t mode[2] = { msgU(desc, "sAddressMode"), msgU(desc, "tAddressMode") };
	uint32_t wrap[2];

	if (o == nil) {
		return nil;
	}
	for (int i = 0; i < 2; i++) {
		wrap[i] = mode[i] == 2 ? 0 : mode[i] == 3 ? 2 : 1;
		if (mode[i] > 3) {
			lavlog("newSamplerState: address mode %llu approximated by clamp to edge", (unsigned long long)mode[i]);
		}
	}
	sp = STATE(o, struct lav5_samp);
	memset(sp, 0, sizeof(*sp));
	sp->mag = msgU(desc, "magFilter") == 1;
	sp->min = msgU(desc, "minFilter") == 1;
	sp->mip_linear = msgU(desc, "mipFilter") == 2;
	sp->wrap_s = wrap[0];
	sp->wrap_t = wrap[1];
	sp->valid = 1;
	if (!(msgU(desc, "normalizedCoordinates") & 0xff)) {
		lavlog("newSamplerState: unnormalized coordinates are not served (ignored)");
	}
	return o;
}

static id sampLabel(__unused id self, __unused SEL _cmd) { return nil; }
static void sampSetLabel(__unused id self, __unused SEL _cmd, __unused id label) { }

static void
rencSetCull(id self, __unused SEL _cmd, uint64_t mode)
{
	STATE(self, struct renc)->cull = (uint32_t)mode;
}

static void
rencSetWinding(id self, __unused SEL _cmd, uint64_t w)
{
	STATE(self, struct renc)->front_cw = w == 0;
}

static void
rencIgnore(__unused id self, __unused SEL _cmd, __unused uint64_t a)
{
}

static void
rencIgnoreObj(__unused id self, __unused SEL _cmd, __unused id a)
{
	static int once;

	if (!once++) {
		lavlog("a render state object (depth/stencil, ...) is not served yet; ignored");
	}
}

static void
rencAddDraw(id self, uint64_t type, uint64_t first, uint64_t count, uint64_t instances, uint64_t base)
{
	struct renc * re = STATE(self, struct renc);
	struct op * op;
	struct draw * d;
	static const uint32_t prims[5] = { 9, 2, 3, 4, 6 };     /* point, line, line strip, triangle, triangle strip */

	if (re->pipeline == nil || type >= 5) {
		lavlog("a draw with no pipeline state, or of primitive type %llu", (unsigned long long)type);
		return;
	}
	op = cbAddOp(STATE(re->commandBuffer, struct cbstate));
	d = calloc(1, sizeof(*d));
	op->kind = OP_DRAW;
	op->draw = d;
	d->pipeline = [re->pipeline retain];
	d->target = [re->target retain];
	for (int i = 1; i < 8; i++) {
		d->xtarget[i] = [re->xtarget[i] retain];
	}
	for (int i = 0; i < LAV5_MAX_BUFFERS; i++) {
		d->vbuf[i] = [re->vbuf[i] retain];
		d->voff[i] = re->voff[i];
		d->fbuf[i] = [re->fbuf[i] retain];
		d->foff[i] = re->foff[i];
		if (re->vbytes[i] != NULL) {
			d->vbytes[i] = malloc(re->vblen[i]);
			memcpy(d->vbytes[i], re->vbytes[i], re->vblen[i]);
			d->vblen[i] = re->vblen[i];
		}
		if (re->fbytes[i] != NULL) {
			d->fbytes[i] = malloc(re->fblen[i]);
			memcpy(d->fbytes[i], re->fbytes[i], re->fblen[i]);
			d->fblen[i] = re->fblen[i];
		}
	}
	d->prim = prims[type];
	d->first = (uint32_t)first;
	d->count = (uint32_t)count;
	d->instances = (uint32_t)instances;
	d->base_instance = (uint32_t)base;
	memcpy(d->viewport, re->viewport, sizeof(d->viewport));
	memcpy(d->scissor, re->scissor, sizeof(d->scissor));
	d->cull = re->cull;
	d->front_cw = re->front_cw;
	for (int i = 0; i < 16; i++) {
		d->ftex[i] = [re->ftex[i] retain];
		d->fsamp[i] = [re->fsamp[i] retain];
	}
	memcpy(d->blend_color, re->blend_color, sizeof(d->blend_color));
}

static void
rencDraw(id self, __unused SEL _cmd, uint64_t type, uint64_t first, uint64_t count)
{
	rencAddDraw(self, type, first, count, 1, 0);
}

static void
rencDrawInstanced(id self, __unused SEL _cmd, uint64_t type, uint64_t first, uint64_t count, uint64_t instances)
{
	rencAddDraw(self, type, first, count, instances, 0);
}

static void
rencDrawBase(id self, __unused SEL _cmd, uint64_t type, uint64_t first, uint64_t count, uint64_t instances, uint64_t base)
{
	rencAddDraw(self, type, first, count, instances, base);
}

static void
rencEnd(id self, __unused SEL _cmd)
{
	STATE(self, struct renc)->ended = YES;
}

static id rencLabel(id self, __unused SEL _cmd) { return STATE(self, struct renc)->label; }

static void
rencSetLabel(id self, __unused SEL _cmd, id label)
{
	struct renc * re = STATE(self, struct renc);

	[re->label release];
	re->label = [label copy];
}

static uint8_t
clamp8(double v)
{
	return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0 + 0.5);
}

static BOOL
runClear(struct op * op)
{
	struct texstate * t = STATE(op->dst, struct texstate);
	uint32_t r = clamp8(op->clear[0]), g = clamp8(op->clear[1]), b = clamp8(op->clear[2]), a = clamp8(op->clear[3]);
	uint32_t px = t->format == PF_BGRA8 ? (b | g << 8 | r << 16 | a << 24) : (r | g << 8 | b << 16 | a << 24);

	for (uint32_t y = 0; y < t->height; y++) {
		uint32_t * row = (uint32_t *)((uint8_t *)t->b.cpu + (size_t)y * t->pitch);

		for (uint32_t x = 0; x < t->width; x++) {
			row[x] = px;
		}
	}
	return YES;
}

static BOOL
bindStage(struct lav5_binding * out, struct lav5_vbuf * vb, id * bufs, uint64_t * offs, uint8_t ** bytes, uint32_t * blen,
    const uint32_t * stride, size_t * boff, const char * name)
{
	for (int i = 0; i < LAV5_MAX_BUFFERS; i++) {
		if (bufs[i] != nil) {
			struct bufstate * b = STATE(bufs[i], struct bufstate);

			if (offs[i] > b->b.len) {
				lavlog("draw of %s: the offset of buffer %d is past its end", name, i);
				return NO;
			}
			out[i].gpu = b->b.gpu + offs[i];
			out[i].cpu = (uint8_t *)b->b.cpu + offs[i];
			out[i].bytes = b->b.len - offs[i];
		} else if (bytes[i] != NULL) {
			uint32_t len = (blen[i] + 15) & ~15u;

			if (*boff + len > gBytes.len) {
				lavlog("draw of %s: too many setBytes", name);
				return NO;
			}
			memset((uint8_t *)gBytes.cpu + *boff, 0, len);
			memcpy((uint8_t *)gBytes.cpu + *boff, bytes[i], blen[i]);
			out[i].gpu = gBytes.gpu + *boff;
			out[i].cpu = (uint8_t *)gBytes.cpu + *boff;
			out[i].bytes = len;
			*boff += len;
		}
		if (vb != NULL) {
			vb[i].gpu = out[i].gpu;
			vb[i].bytes = out[i].bytes;
			vb[i].stride = stride[i];
		}
	}
	return YES;
}

static BOOL
runDraw(struct op * op)
{
	struct draw * d = op->draw;
	struct rpipe * rp = STATE(d->pipeline, struct rpipe);
	struct texstate * t = STATE(d->target, struct texstate);
	struct lav5_render * r = calloc(1, sizeof(*r));
	uint32_t * w;
	const char * why = NULL;
	size_t n, boff = 0;
	BOOL ok;
	const char * name = rp->vs.k.name[0] ? rp->vs.k.name : "(render)";

	if (gBytes.handle == 0 && !lavBufNew(65536, &gBytes)) {
		free(r);
		return NO;
	}
	r->vs = &rp->vs;
	r->fs = &rp->fs;
	r->link = &rp->link;
	r->vs_code = rp->vcode.gpu;
	r->fs_code = rp->fcode.gpu;
	r->rt_gpu = t->b.gpu;
	r->rt_width = t->width;
	r->rt_height = t->height;
	r->rt_pitch = t->pitch;
	r->rt_format = t->format == PF_BGRA8 ? 0 : 1;
	r->nattr = rp->nattr;
	memcpy(r->attr, rp->attr, sizeof(r->attr));
	if (!bindStage(r->vs_buf, r->vbuf, d->vbuf, d->voff, d->vbytes, d->vblen, rp->stride, &boff, name) ||
	    !bindStage(r->fs_buf, NULL, d->fbuf, d->foff, d->fbytes, d->fblen, NULL, &boff, name)) {
		free(r);
		return NO;
	}
	r->prim = d->prim;
	r->first = d->first;
	r->count = d->count;
	r->instances = d->instances;
	r->base_instance = d->base_instance;
	memcpy(r->viewport, d->viewport, sizeof(r->viewport));
	memcpy(r->scissor, d->scissor, sizeof(r->scissor));
	r->cull = d->cull;
	r->front_cw = d->front_cw;
	r->blend = rp->blend[0];
	r->nrt = 1;
	for (int i = 1; i < 8; i++) {
		r->blend_x[i] = rp->blend[i];
		if (d->xtarget[i] != nil) {
			struct texstate * xt = STATE(d->xtarget[i], struct texstate);

			r->rt[i] = (struct lav5_rt){ xt->b.gpu, xt->pitch, xt->format == PF_BGRA8 ? 0u : 1u, 1 };
			r->nrt = i + 1;
		}
	}
	r->rt[0] = (struct lav5_rt){ r->rt_gpu, r->rt_pitch, r->rt_format, 1 };
	memcpy(r->blend_color, d->blend_color, sizeof(r->blend_color));
	for (int i = 0; i < 16; i++) {
		if (d->ftex[i] != nil) {
			struct texstate * ts = STATE(d->ftex[i], struct texstate);

			r->tex[i] = (struct lav5_tex){ ts->b.gpu, ts->width, ts->height, ts->pitch, ts->format == PF_BGRA8, 1 };
		}
		if (d->fsamp[i] != nil) {
			r->samp[i] = *STATE(d->fsamp[i], struct lav5_samp);
		}
	}
	r->flush_gpu = gStream.gpu + SCRATCH_OFFSET + 64;
	r->hole_gpu = gStream.gpu + SCRATCH_OFFSET + 128;
	w = malloc(STREAM_WORDS * 4);
	n = lav5_build_render(w, STREAM_WORDS, r, &why);
	if (n == 0) {
		lavlog("draw refused: %s", why != NULL ? why : "?");
		free(w);
		free(r);
		return NO;
	}
	ok = lavSubmit(w, n);
	lavlog("draw: %u vertices x %u instances, prim %u, %zu words, %s", d->count, d->instances, d->prim, n, ok ? "done" : "FAILED");
	free(w);
	free(r);
	return ok;
}
