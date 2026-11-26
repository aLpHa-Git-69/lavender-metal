/* LavenderRender.inc: M5.4 - textures, render pipeline states and the render encoder. Included by LavenderMetal.m (one
 * translation unit), after the compute classes and before the device. One draw is one stream (lava5r.c), sysmem mode; the
 * colour target is a texture the encoder cleared (by the CPU) or loaded. No depth, blending or sampling yet; what is not
 * served is refused by name in the log. */

typedef struct { double x, y, w, h, znear, zfar; } LavViewport;
typedef struct { uint64_t x, y, w, h; } LavScissor;
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

#define PF_A8         1
#define PF_R8         10
#define PF_R16        23
#define PF_R16F       25
#define PF_RG8        30
#define PF_B5G6R5     40
#define PF_BGR5A1     43
#define PF_R32F       55
#define PF_RGBA8      70
#define PF_RGBA8_SRGB 71
#define PF_BGRA8      80
#define PF_BGRA8_SRGB 81
#define PF_RG32F      105
#define PF_RGBA16F    115

static uint32_t
lavFmtFromMetal(uint32_t mtlFmt, uint32_t * bpp)
{
	switch (mtlFmt) {
	case PF_A8:         if (bpp) *bpp = 1; return LAV5_FMT_A8;
	case PF_R8:         if (bpp) *bpp = 1; return LAV5_FMT_R8;
	case PF_R16:        if (bpp) *bpp = 2; return LAV5_FMT_R16;
	case PF_R16F:       if (bpp) *bpp = 2; return LAV5_FMT_R16F;
	case PF_RG8:        if (bpp) *bpp = 2; return LAV5_FMT_RG8;
	case PF_B5G6R5:     if (bpp) *bpp = 2; return LAV5_FMT_B5G6R5;
	case PF_BGR5A1:     if (bpp) *bpp = 2; return LAV5_FMT_BGR5A1;
	case PF_R32F:       if (bpp) *bpp = 4; return LAV5_FMT_R32F;
	case PF_RGBA8:      if (bpp) *bpp = 4; return LAV5_FMT_RGBA8;
	case PF_RGBA8_SRGB: if (bpp) *bpp = 4; return LAV5_FMT_RGBA8_SRGB;
	case PF_BGRA8:      if (bpp) *bpp = 4; return LAV5_FMT_BGRA8;
	case PF_BGRA8_SRGB: if (bpp) *bpp = 4; return LAV5_FMT_BGRA8_SRGB;
	case PF_RG32F:      if (bpp) *bpp = 8; return LAV5_FMT_RG32F;
	case PF_RGBA16F:    if (bpp) *bpp = 8; return LAV5_FMT_RGBA16F;
	default:            if (bpp) *bpp = 0; return 0xffu;
	}
}

struct texstate {
	IOSurfaceRef surface;                   /* retained: the texture is backed by an IOSurface; sync on read/present */
	id backingBuffer;                       /* retained: buffer-backed linear texture (newTextureWithDescriptor:offset:bytesPerRow:) */
	uint64_t backingOffset, backingPitch;
	BOOL backingAlias;
	struct lbuf b;
	uint32_t width, height, format, pitch;
	uint32_t mips, bpp, texType, lastSeed;
	BOOL seedValid;
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
	uint32_t bpp = 0;
	BOOL ok;

	if (type == 0 && h == 0) {
		h = 1;
	}
	if (!lavOpen() || w == 0 || h == 0 || w > 4096 || h > 4096) {
		lavlog("newTexture %llux%llu refused", (unsigned long long)w, (unsigned long long)h);
		return nil;
	}
	if (lavFmtFromMetal((uint32_t)fmt, &bpp) == 0xffu || (type != 2 && type != 0) || depth > 1 || samples > 1 || layers > 1) {
		lavlog("newTexture: pixel format %llu, type %llu, %llu mips, %llu samples, %llu layers: not served",
		    (unsigned long long)fmt, (unsigned long long)type, (unsigned long long)mips, (unsigned long long)samples,
		    (unsigned long long)layers);
		return nil;
	}
	pitch = ((size_t)w * bpp + 63) & ~(size_t)63;
	len = pitch * (size_t)h;
	if (mips > 1) {
		/* room for the whole chain (under 4/3 of the base level); only level 0 is drawn to and sampled for now */
		len = len + len / 2;
	}
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
		[o release];
		return nil;
	}
	memset(st->b.cpu, 0, st->b.len);
	st->width = (uint32_t)w;
	st->height = (uint32_t)h;
	st->format = (uint32_t)fmt;
	st->pitch = (uint32_t)pitch;
	st->mips = mips > 0 ? (uint32_t)mips : 1u;
	st->bpp = bpp;
	st->texType = (uint32_t)type;
	st->usage = msgU(desc, "usage");
	st->storage = msgU(desc, "storageMode");
	lavdbg("texture %llux%llu format %llu, pitch %zu, at GPU %#llx", (unsigned long long)w, (unsigned long long)h,
	    (unsigned long long)fmt, pitch, (unsigned long long)st->b.gpu);
	return o;
}

/* IOSurface <-> the texture's own memory (the GPU cannot reach an IOSurface's scattered pages): rows copied at the edges */
static void
surfaceCopy(struct texstate * t, BOOL toSurface)
{
	size_t sp, rows = t->height, row = (size_t)t->width * t->bpp;
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
	if (sp < row) {
		row = sp;
	}
	for (size_t y = 0; y < rows; y++) {
		if (toSurface) {
			memcpy(base + y * sp, (uint8_t *)t->b.cpu + y * t->pitch, row);
		} else {
			memcpy((uint8_t *)t->b.cpu + y * t->pitch, base + y * sp, row);
		}
	}
	IOSurfaceUnlock(t->surface, toSurface ? 0 : kIOSurfaceLockReadOnly, NULL);
	t->lastSeed = IOSurfaceGetSeed(t->surface);
	t->seedValid = YES;
}

/* Pull IOSurface or unaligned backing buffer bytes into GPU memory when modified externally. */
static void
surfacePullIfDirty(struct texstate * t)
{
	if (t->backingBuffer != nil && !t->backingAlias) {
		struct bufstate * bs = STATE(t->backingBuffer, struct bufstate);
		size_t row = (size_t)t->width * t->bpp;

		if (t->backingPitch < row) {
			row = (size_t)t->backingPitch;
		}
		for (uint32_t y = 0; y < t->height; y++) {
			memcpy((uint8_t *)t->b.cpu + (size_t)y * t->pitch,
			    (const uint8_t *)bs->b.cpu + t->backingOffset + (size_t)y * t->backingPitch, row);
		}
		return;
	}
	if (t->surface == NULL) {
		return;
	}
	if (!t->seedValid || IOSurfaceGetSeed(t->surface) != t->lastSeed) {
		surfaceCopy(t, NO);
	}
}

static void
cbSyncSurfaces(struct cbstate * st)
{
	static uint32_t syncLog;
	id synced[32];
	size_t nsynced = 0;

	for (size_t i = 0; i < st->nops; i++) {
		id cand[8] = { nil };
		size_t ncand = 0;

		if (st->ops[i].kind == OP_DRAW && st->ops[i].draw != NULL) {
			cand[ncand++] = st->ops[i].draw->target;
			for (int j = 1; j < 8; j++) {
				if (st->ops[i].draw->xtarget[j] != nil) {
					cand[ncand++] = st->ops[i].draw->xtarget[j];
				}
			}
		} else if (st->ops[i].kind == OP_CLEAR) {
			cand[ncand++] = st->ops[i].dst;
		} else if (st->ops[i].kind == OP_COPY_TEX && st->ops[i].copytex != NULL) {
			cand[ncand++] = st->ops[i].copytex->dst;
		}
		for (size_t c = 0; c < ncand; c++) {
			id tex = cand[c];
			BOOL seen = NO;
			struct texstate * ts;

			if (tex == nil || STATE(tex, struct texstate)->surface == NULL) {
				continue;
			}
			for (size_t s = 0; s < nsynced; s++) {
				if (synced[s] == tex) {
					seen = YES;
					break;
				}
			}
			if (seen) {
				continue;
			}
			if (nsynced < 32) {
				synced[nsynced++] = tex;
			}
			ts = STATE(tex, struct texstate);
			surfaceCopy(ts, YES);
			if (syncLog < 12 && ts->bpp == 4 && ts->width >= 2 && ts->height >= 2) {
				const uint8_t * p = (const uint8_t *)ts->b.cpu;
				uint32_t p0 = 0, pm = 0, pe = 0;

				memcpy(&p0, p, 4);
				memcpy(&pm, p + (size_t)(ts->height / 2) * ts->pitch + (size_t)(ts->width / 2) * 4, 4);
				memcpy(&pe, p + (size_t)(ts->height - 1) * ts->pitch + (size_t)(ts->width - 1) * 4, 4);
				lavlog("sync #%u %ux%u fmt %u px[tl,mid,br]: %08x %08x %08x",
				    syncLog, ts->width, ts->height, ts->format, p0, pm, pe);
				syncLog++;
			}
		}
	}
}

static id
deviceNewTextureSurface(id self, SEL cmd, id desc, IOSurfaceRef surface, uint64_t plane)
{
	id o;
	struct texstate * st;
	uint32_t sfmt;

	if (surface == NULL || plane != 0) {
		lavlog("newTexture from an IOSurface: plane %llu refused", (unsigned long long)plane);
		return nil;
	}
	sfmt = IOSurfaceGetPixelFormat(surface);
	if (sfmt != 'BGRA' && sfmt != 'RGBA' && sfmt != 'L008' && sfmt != 'LA08' && sfmt != 'RGhA') {
		lavlog("newTexture from an IOSurface: pixel format %#x: only BGRA/RGBA/L008/LA08/RGhA served", sfmt);
		return nil;
	}
	o = deviceNewTexture(self, cmd, desc);
	if (o == nil) {
		return nil;
	}
	st = STATE(o, struct texstate);
	if (IOSurfaceGetWidth(surface) < st->width || IOSurfaceGetHeight(surface) < st->height) {
		lavlog("newTexture from an IOSurface: the surface (%zux%zu) does not hold the texture (%ux%u)", IOSurfaceGetWidth(surface),
		    IOSurfaceGetHeight(surface), st->width, st->height);
		[o release];
		return nil;
	}
	st->surface = (IOSurfaceRef)CFRetain(surface);
	st->seedValid = NO;
	lavdbg("texture over an IOSurface %zux%zu fmt %#x, row %zu bytes", IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
	    sfmt, IOSurfaceGetBytesPerRow(surface));
	return o;
}

static id
bufferNewTexture(id self, __unused SEL _cmd, id desc, uint64_t offset, uint64_t bytesPerRow)
{
	struct bufstate * bs = STATE(self, struct bufstate);
	uint64_t w = msgU(desc, "width"), h = msgU(desc, "height"), fmt = msgU(desc, "pixelFormat"), type = msgU(desc, "textureType");
	uint64_t depth = msgU(desc, "depth"), samples = msgU(desc, "sampleCount"), layers = msgU(desc, "arrayLength");
	uint32_t bpp = 0;
	struct texstate * st;
	id o;

	if (type == 0 && h == 0) {
		h = 1;
	}
	if (!lavOpen() || w == 0 || h == 0 || w > 4096 || h > 4096 ||
	    lavFmtFromMetal((uint32_t)fmt, &bpp) == 0xffu || (type != 2 && type != 0) || depth > 1 || samples > 1 || layers > 1) {
		lavlog("buffer newTexture: %llux%llu fmt %llu type %llu refused",
		    (unsigned long long)w, (unsigned long long)h, (unsigned long long)fmt, (unsigned long long)type);
		return nil;
	}
	if (bytesPerRow < w * bpp || offset > bs->b.len || bytesPerRow * h > bs->b.len - offset) {
		lavlog("buffer newTexture: offset %llu + %llu*%llu past buffer len %zu",
		    (unsigned long long)offset, (unsigned long long)bytesPerRow, (unsigned long long)h, bs->b.len);
		return nil;
	}
	o = makeObject(gTextureClass, sizeof(struct texstate));
	if (o == nil) {
		return nil;
	}
	st = STATE(o, struct texstate);
	memset(st, 0, sizeof(*st));
	st->backingBuffer = [self retain];
	st->backingOffset = offset;
	st->backingPitch = bytesPerRow;
	if ((bytesPerRow & 63u) == 0 && ((bs->b.gpu + offset) & 63u) == 0) {
		st->backingAlias = YES;
		st->b.gpu = bs->b.gpu + offset;
		st->b.cpu = (uint8_t *)bs->b.cpu + offset;
		st->b.len = (size_t)(bytesPerRow * h);
		st->pitch = (uint32_t)bytesPerRow;
	} else {
		size_t pitch = ((size_t)w * bpp + 63u) & ~(size_t)63u;
		size_t len = pitch * (size_t)h;
		BOOL ok;

		pthread_mutex_lock(&gGPULock);
		ok = lavBufNew((len + 4095u) & ~(size_t)4095u, &st->b);
		pthread_mutex_unlock(&gGPULock);
		if (!ok) {
			[o release];
			return nil;
		}
		st->backingAlias = NO;
		st->pitch = (uint32_t)pitch;
	}
	st->width = (uint32_t)w;
	st->height = (uint32_t)h;
	st->format = (uint32_t)fmt;
	st->mips = 1;
	st->bpp = bpp;
	st->texType = (uint32_t)type;
	st->usage = msgU(desc, "usage");
	st->storage = msgU(desc, "storageMode");
	return o;
}

static id
bufferNewLinearTexture(id self, SEL cmd, id desc, uint64_t offset, uint64_t bytesPerRow, __unused uint64_t bytesPerImage)
{
	return bufferNewTexture(self, cmd, desc, offset, bytesPerRow);
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

	if (!st->backingAlias && st->b.handle != 0) {
		pthread_mutex_lock(&gGPULock);
		lavBufFree(&st->b);
		pthread_mutex_unlock(&gGPULock);
	}
	[st->backingBuffer release];
	if (st->surface != NULL) {
		CFRelease(st->surface);
	}
	[st->label release];
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static IOSurfaceRef texIOSurface(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->surface; }
static uint64_t texWidth(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->width; }
static uint64_t texHeight(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->height; }
static uint64_t texZero(__unused id self, __unused SEL _cmd) { return 0; }
static uint64_t texOne(__unused id self, __unused SEL _cmd) { return 1; }
static uint64_t texMips(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->mips; }
static uint64_t texType(id self, __unused SEL _cmd) { return STATE(self, struct texstate)->texType; }
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
	surfacePullIfDirty(st);
	for (uint64_t y = 0; y < r.size.height; y++) {
		memcpy((uint8_t *)dst + y * pitch, (uint8_t *)st->b.cpu + (r.origin.y + y) * st->pitch + r.origin.x * st->bpp,
		    r.size.width * st->bpp);
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
		memcpy((uint8_t *)st->b.cpu + (r.origin.y + y) * st->pitch + r.origin.x * st->bpp, (const uint8_t *)src + y * pitch,
		    r.size.width * st->bpp);
	}
}

static void
texReplaceSlice(id self, SEL cmd, LavRegion r, uint64_t level, __unused uint64_t slice, const void * src, uint64_t pitch, __unused uint64_t image)
{
	texReplace(self, cmd, r, level, src, pitch);
}

/* ---- the render pipeline state ---- */

#define RPIPE_GLOBAL_SPECS 128u

struct rpipe_spec {
	uint64_t hash;
	uint32_t nbufs;
	uint32_t bindings[LAVCC_MAX_UBER_BUFS];
	uint32_t lens[LAVCC_MAX_UBER_BUFS];
	uint8_t keys[LAVCC_MAX_UBER_BUFS][LAVCC_UBER_BUF_CAP];
	BOOL failed;
	struct lav5_stage vs, fs;
	struct lav5_link link;
	struct lbuf vcode, fcode;
};

static struct rpipe_spec gSpecCache[RPIPE_GLOBAL_SPECS];
static uint32_t gSpecCount;

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
	/* uber-shader specialization on small constant buffers (e.g. CoreAnimation's FragmentShaderConfig) */
	BOOL fs_deferred;
	uint32_t spec_nbufs;
	struct lavcc_uber_buf spec_bufs[LAVCC_MAX_UBER_BUFS];
};

static BOOL
codeBufLocked(struct lbuf * b, const struct lav5_kernel * k)
{
	size_t len = (size_t)k->instrlen * 128;
	BOOL ok;

	if (len < (size_t)k->nwords * 4) {
		len = (size_t)k->nwords * 4;
	}
	ok = lavBufNew((len + 4095) & ~(size_t)4095, b);
	if (ok) {
		memset(b->cpu, 0, b->len);
		memcpy(b->cpu, k->words, (size_t)k->nwords * 4);
	}
	return ok;
}

static BOOL
codeBuf(struct lbuf * b, const struct lav5_kernel * k)
{
	BOOL ok;

	pthread_mutex_lock(&gGPULock);
	ok = codeBufLocked(b, k);
	pthread_mutex_unlock(&gGPULock);
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
	case 25: *fmt = 69; return YES;
	case 26: *fmt = 90; return YES;
	case 27: *fmt = 98; return YES;
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

/*
 * Compiling a pipeline takes seconds (QuartzCore's fixed_frag_lph_cph is 26000 words), and backboardd needs dozens at start. The
 * compiled stages are kept in /var/tmp/lavmetal-cache, one file per vertex/fragment pair, keyed by the two metallibs and the
 * function names and by this plug-in build (the compiler changes with it).
 */
#define PIPE_CACHE_DIR "/var/tmp/lavmetal-cache"
#define PIPE_CACHE_MAGIC 0x4c564336u

static uint64_t
pipeKeyHash(struct fnstate * vs, struct fnstate * fs, const struct lavcc_uber_val * uvals, uint32_t nuvals)
{
	uint64_t h = 0xcbf29ce484222325ull;
	const char * stamp = __DATE__ __TIME__;

	h = lavHash64(h, stamp, strlen(stamp));
	h = lavHash64(h, &STATE(vs->library, struct libstate)->hash, 8);
	h = lavHash64(h, &STATE(fs->library, struct libstate)->hash, 8);
	h = lavHash64(h, [vs->name UTF8String], strlen([vs->name UTF8String]) + 1);
	h = lavHash64(h, [fs->name UTF8String], strlen([fs->name UTF8String]) + 1);
	h = lavHash64(h, &vs->nfcs, sizeof(vs->nfcs));
	if (vs->nfcs > 0 && vs->fcs != NULL) {
		h = lavHash64(h, vs->fcs, vs->nfcs * sizeof(*vs->fcs));
	}
	h = lavHash64(h, &fs->nfcs, sizeof(fs->nfcs));
	if (fs->nfcs > 0 && fs->fcs != NULL) {
		h = lavHash64(h, fs->fcs, fs->nfcs * sizeof(*fs->fcs));
	}
	if (uvals != NULL && nuvals > 0) {
		h = lavHash64(h, &nuvals, sizeof(nuvals));
		for (uint32_t i = 0; i < nuvals; i++) {
			h = lavHash64(h, &uvals[i].binding, sizeof(uvals[i].binding));
			h = lavHash64(h, &uvals[i].len, sizeof(uvals[i].len));
			if (uvals[i].bytes != NULL && uvals[i].len > 0) {
				h = lavHash64(h, uvals[i].bytes, uvals[i].len);
			}
		}
	}
	return h;
}

static void
pipeCachePathSpec(struct fnstate * vs, struct fnstate * fs, const struct lavcc_uber_val * uvals, uint32_t nuvals,
    char * path, size_t n)
{
	uint64_t h = pipeKeyHash(vs, fs, uvals, nuvals);

	snprintf(path, n, PIPE_CACHE_DIR "/%016llx.pipe", (unsigned long long)h);
}

static BOOL
pipeReadStage(FILE * f, struct lav5_stage * st)
{
	uint32_t * w, * imm;
	uint32_t nw, ni;

	if (fread(st, sizeof(*st), 1, f) != 1) {
		return NO;
	}
	nw = st->k.nwords;
	ni = st->k.nimm;
	if (nw == 0 || nw > (1u << 20) || ni > (1u << 16)) {
		return NO;
	}
	w = malloc(nw * 4);
	imm = ni != 0 ? malloc(ni * 4) : NULL;
	if (fread(w, 4, nw, f) != nw || (ni != 0 && fread(imm, 4, ni, f) != ni)) {
		free(w);
		free(imm);
		return NO;
	}
	st->k.words = w;
	st->k.imm = imm;
	return YES;
}

static BOOL
pipeCacheLoadSpec(struct fnstate * vs, struct fnstate * fs, const struct lavcc_uber_val * uvals, uint32_t nuvals,
    struct lav5_stage * out_vs, struct lav5_stage * out_fs, struct lav5_link * out_link,
    struct lavcc_uber_buf * out_ubufs, uint32_t * out_nubufs)
{
	char path[160];
	uint32_t magic = 0, nb = 0;
	FILE * f;
	BOOL ok;

	pipeCachePathSpec(vs, fs, uvals, nuvals, path, sizeof(path));
	f = fopen(path, "rb");
	if (f == NULL) {
		return NO;
	}
	ok = fread(&magic, 4, 1, f) == 1 && magic == PIPE_CACHE_MAGIC && fread(out_link, sizeof(*out_link), 1, f) == 1 &&
	    pipeReadStage(f, out_vs) && pipeReadStage(f, out_fs);
	if (ok && nuvals == 0) {
		ok = fread(&nb, sizeof(nb), 1, f) == 1 && nb <= LAVCC_MAX_UBER_BUFS &&
		    (nb == 0 || (out_ubufs != NULL && fread(out_ubufs, sizeof(*out_ubufs), nb, f) == nb));
		if (ok && out_nubufs != NULL) {
			*out_nubufs = nb;
		}
	}
	fclose(f);
	if (!ok) {
		lavcc_stage_free(out_vs);
		lavcc_stage_free(out_fs);
		memset(out_vs, 0, sizeof(*out_vs));
		memset(out_fs, 0, sizeof(*out_fs));
		return NO;
	}
	if (nuvals == 0) {
		lavlog("pipeline cache: %s + %s (fc %u+%u, spec bufs %u)", [vs->name UTF8String], [fs->name UTF8String],
		    vs->nfcs, fs->nfcs, nb);
	}
	return YES;
}

static BOOL
pipeCacheLoad(struct fnstate * vs, struct fnstate * fs, struct rpipe * rp)
{
	return pipeCacheLoadSpec(vs, fs, NULL, 0, &rp->vs, &rp->fs, &rp->link, rp->spec_bufs, &rp->spec_nbufs);
}

static void
pipeWriteStage(FILE * f, const struct lav5_stage * st)
{
	struct lav5_stage c = *st;

	c.k.words = NULL;
	c.k.imm = NULL;
	fwrite(&c, sizeof(c), 1, f);
	fwrite(st->k.words, 4, st->k.nwords, f);
	if (st->k.nimm != 0) {
		fwrite(st->k.imm, 4, st->k.nimm, f);
	}
}

static void
pipeCacheStoreSpec(struct fnstate * vs, struct fnstate * fs, const struct lavcc_uber_val * uvals, uint32_t nuvals,
    const struct lav5_stage * st_vs, const struct lav5_stage * st_fs, const struct lav5_link * st_link,
    const struct lavcc_uber_buf * ubufs, uint32_t nubufs)
{
	char path[160], tmp[176];
	uint32_t magic = PIPE_CACHE_MAGIC;
	FILE * f;

	mkdir(PIPE_CACHE_DIR, 01777);
	chmod(PIPE_CACHE_DIR, 01777);
	pipeCachePathSpec(vs, fs, uvals, nuvals, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid());
	f = fopen(tmp, "wb");
	if (f == NULL) {
		return;
	}
	fwrite(&magic, 4, 1, f);
	fwrite(st_link, sizeof(*st_link), 1, f);
	pipeWriteStage(f, st_vs);
	pipeWriteStage(f, st_fs);
	if (nuvals == 0) {
		fwrite(&nubufs, sizeof(nubufs), 1, f);
		if (nubufs > 0 && ubufs != NULL) {
			fwrite(ubufs, sizeof(*ubufs), nubufs, f);
		}
	}
	if (fclose(f) == 0) {
		rename(tmp, path);
	} else {
		unlink(tmp);
	}
}

static void
pipeCacheStore(struct fnstate * vs, struct fnstate * fs, struct rpipe * rp)
{
	pipeCacheStoreSpec(vs, fs, NULL, 0, &rp->vs, &rp->fs, &rp->link, rp->spec_bufs, rp->spec_nbufs);
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
	ok = pipeCacheLoad(vs, fs, rp);
	if (!ok) {
		const char * fname = [fs->name UTF8String];
		BOOL deferred = NO;

		if (strncmp(fname, "fixed_frag_", 11) == 0) {
			uint32_t nb = 0, nifs = 0;
			char sum[256] = "", uwhy[256] = "";

			if (lavcc_uber_info(STATE(fs->library, struct libstate)->lib, fname, fs->fcs, fs->nfcs,
			    rp->spec_bufs, &nb, &nifs, sum, sizeof(sum), uwhy, sizeof(uwhy)) && nifs >= 64 && nb > 0) {
				ok = lavcc_compile_vs(STATE(vs->library, struct libstate)->lib, [vs->name UTF8String], vs->fcs, vs->nfcs,
				    &rp->vs, why, sizeof(why));
				if (ok) {
					rp->spec_nbufs = nb;
					rp->fs_deferred = YES;
					deferred = YES;
					lavlog("render pipeline %s + %s (fc %u+%u): deferred uber-shader (vs %u words), ubos: %s",
					    [vs->name UTF8String], fname, vs->nfcs, fs->nfcs, rp->vs.k.nwords, sum);
				}
			}
		}
		if (!deferred && !ok) {
			ok = lavcc_compile_program(STATE(vs->library, struct libstate)->lib, [vs->name UTF8String], vs->fcs, vs->nfcs,
			    STATE(fs->library, struct libstate)->lib, fname, fs->fcs, fs->nfcs,
			    &rp->vs, &rp->fs, &rp->link, why, sizeof(why));
			if (ok && rp->fs.k.nwords >= 4096) {
				uint32_t nb = 0, nifs = 0;
				char sum[256] = "", uwhy[256] = "";

				if (lavcc_uber_info(STATE(fs->library, struct libstate)->lib, fname, fs->fcs, fs->nfcs,
				    rp->spec_bufs, &nb, &nifs, sum, sizeof(sum), uwhy, sizeof(uwhy)) && nb > 0) {
					rp->spec_nbufs = nb;
					lavlog("render pipeline %s + %s (fc %u+%u): uber-shader (%u fs words), ubos: %s",
					    [vs->name UTF8String], fname, vs->nfcs, fs->nfcs, rp->fs.k.nwords, sum);
				} else {
					lavlog("render pipeline %s + %s (fc %u+%u): uber-shader (%u fs words), no UBO spec: %s",
					    [vs->name UTF8String], fname, vs->nfcs, fs->nfcs, rp->fs.k.nwords, uwhy);
				}
			}
			if (ok) {
				pipeCacheStore(vs, fs, rp);
			}
		}
	}
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
	if (!codeBuf(&rp->vcode, &rp->vs.k) || (!rp->fs_deferred && !codeBuf(&rp->fcode, &rp->fs.k))) {
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
	lavlog("render pipeline %s + %s: %u+%u words%s, %u attributes", [vs->name UTF8String], [fs->name UTF8String], rp->vs.k.nwords,
	    rp->fs.k.nwords, rp->fs_deferred ? " (deferred)" : "", rp->nattr);
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

	lavdbg("render pipeline freed (code handles %u, %u)", st->vcode.handle, st->fcode.handle);
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
		surfacePullIfDirty(ts);
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
rencAddDraw(id self, uint64_t type, uint64_t first, uint64_t count, uint64_t instances, uint64_t base, id ibuf, uint64_t ioff,
    uint32_t isize)
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
	d->ibuf = [ibuf retain];
	d->ioff = ioff;
	d->isize = isize;
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
	rencAddDraw(self, type, first, count, 1, 0, nil, 0, 0);
}

static void
rencDrawInstanced(id self, __unused SEL _cmd, uint64_t type, uint64_t first, uint64_t count, uint64_t instances)
{
	rencAddDraw(self, type, first, count, instances, 0, nil, 0, 0);
}

static void
rencDrawBase(id self, __unused SEL _cmd, uint64_t type, uint64_t first, uint64_t count, uint64_t instances, uint64_t base)
{
	rencAddDraw(self, type, first, count, instances, base, nil, 0, 0);
}

static void
rencDrawIndexedBase(id self, __unused SEL _cmd, uint64_t type, uint64_t count, uint64_t itype, id ibuf, uint64_t ioff,
    uint64_t instances, int64_t basevertex, uint64_t base)
{
	/* the vertex offset (VFD_INDEX_OFFSET) is the base vertex */
	rencAddDraw(self, type, (uint32_t)basevertex, count, instances, base, ibuf, ioff, itype == 0 ? 1 : 2);
}

static void
rencDrawIndexed(id self, SEL cmd, uint64_t type, uint64_t count, uint64_t itype, id ibuf, uint64_t ioff)
{
	rencDrawIndexedBase(self, cmd, type, count, itype, ibuf, ioff, 1, 0, 0);
}

static void
rencDrawIndexedInst(id self, SEL cmd, uint64_t type, uint64_t count, uint64_t itype, id ibuf, uint64_t ioff, uint64_t instances)
{
	rencDrawIndexedBase(self, cmd, type, count, itype, ibuf, ioff, instances, 0, 0);
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

static uint16_t
f32ToF16(float f)
{
	uint32_t u;
	uint32_t sign, exp, mant;

	memcpy(&u, &f, 4);
	sign = (u >> 16) & 0x8000u;
	exp = (u >> 23) & 0xffu;
	mant = u & 0x7fffffu;
	if (exp == 255) {
		return (uint16_t)(sign | 0x7c00u | (mant ? 0x0200u : 0u));
	}
	if (exp > 142) {
		return (uint16_t)(sign | 0x7c00u);
	}
	if (exp < 113) {
		if (exp < 103) {
			return (uint16_t)sign;
		}
		mant |= 0x800000u;
		return (uint16_t)(sign | (mant >> (126 - exp)));
	}
	return (uint16_t)(sign | ((exp - 112) << 10) | (mant >> 13));
}

static BOOL
runClear(struct op * op)
{
	struct texstate * t = STATE(op->dst, struct texstate);
	uint32_t r = clamp8(op->clear[0]), g = clamp8(op->clear[1]), b = clamp8(op->clear[2]), a = clamp8(op->clear[3]);

	if (t->bpp == 1) {
		uint8_t v = (uint8_t)(t->format == PF_A8 ? a : r);

		for (uint32_t y = 0; y < t->height; y++) {
			memset((uint8_t *)t->b.cpu + (size_t)y * t->pitch, v, t->width);
		}
	} else if (t->bpp == 2) {
		uint16_t px = 0;

		if (t->format == PF_RG8) {
			px = (uint16_t)(r | (g << 8));
		} else if (t->format == PF_R16) {
			double v = op->clear[0];
			px = (uint16_t)(v <= 0 ? 0 : v >= 1 ? 65535 : (uint32_t)(v * 65535.0 + 0.5));
		} else if (t->format == PF_R16F) {
			px = f32ToF16((float)op->clear[0]);
		} else if (t->format == PF_B5G6R5) {
			px = (uint16_t)(((b >> 3) & 0x1fu) | (((g >> 2) & 0x3fu) << 5) | (((r >> 3) & 0x1fu) << 11));
		} else if (t->format == PF_BGR5A1) {
			px = (uint16_t)(((b >> 3) & 0x1fu) | (((g >> 3) & 0x1fu) << 5) | (((r >> 3) & 0x1fu) << 10) | ((a >= 128 ? 1u : 0u) << 15));
		}
		for (uint32_t y = 0; y < t->height; y++) {
			uint16_t * row = (uint16_t *)((uint8_t *)t->b.cpu + (size_t)y * t->pitch);

			for (uint32_t x = 0; x < t->width; x++) {
				row[x] = px;
			}
		}
	} else if (t->bpp == 4) {
		uint32_t px;
		size_t rowBytes = (size_t)t->width * 4;

		if (t->format == PF_R32F) {
			float f = (float)op->clear[0];
			memcpy(&px, &f, 4);
		} else {
			px = (t->format == PF_BGRA8 || t->format == PF_BGRA8_SRGB) ?
			    (b | g << 8 | r << 16 | a << 24) : (r | g << 8 | b << 16 | a << 24);
		}
		for (uint32_t y = 0; y < t->height; y++) {
			memset_pattern4((uint8_t *)t->b.cpu + (size_t)y * t->pitch, &px, rowBytes);
		}
	} else if (t->bpp == 8) {
		uint64_t px = 0;
		size_t rowBytes = (size_t)t->width * 8;

		if (t->format == PF_RG32F) {
			float f[2] = { (float)op->clear[0], (float)op->clear[1] };
			memcpy(&px, f, 8);
		} else {
			uint16_t h[4] = { f32ToF16((float)op->clear[0]), f32ToF16((float)op->clear[1]),
			    f32ToF16((float)op->clear[2]), f32ToF16((float)op->clear[3]) };
			memcpy(&px, h, 8);
		}
		for (uint32_t y = 0; y < t->height; y++) {
			memset_pattern8((uint8_t *)t->b.cpu + (size_t)y * t->pitch, &px, rowBytes);
		}
	}
	return YES;
}

static BOOL
runCopyTex(struct op * op)
{
	struct copytex * ct = op->copytex;
	struct texstate * s, * d;

	if (ct == NULL || ct->src == nil || ct->dst == nil) {
		return YES;
	}
	if (object_getClass(ct->src) != gTextureClass || object_getClass(ct->dst) != gTextureClass) {
		return YES;
	}
	s = STATE(ct->src, struct texstate);
	d = STATE(ct->dst, struct texstate);
	if (ct->slevel != 0 || ct->dlevel != 0) {
		return YES;
	}
	if (s->bpp != d->bpp || ct->sx + ct->w > s->width || ct->sy + ct->h > s->height ||
	    ct->dx + ct->w > d->width || ct->dy + ct->h > d->height) {
		lavlog("copyFromTexture: bounds/bpp mismatch (%ux%u bpp %u -> %ux%u bpp %u)",
		    s->width, s->height, s->bpp, d->width, d->height, d->bpp);
		return NO;
	}
	surfacePullIfDirty(s);
	for (uint64_t y = 0; y < ct->h; y++) {
		memcpy((uint8_t *)d->b.cpu + (ct->dy + y) * d->pitch + ct->dx * d->bpp,
		    (const uint8_t *)s->b.cpu + (ct->sy + y) * s->pitch + ct->sx * s->bpp,
		    (size_t)ct->w * s->bpp);
	}
	return YES;
}

static BOOL
pipeEnsureBaseLocked(struct rpipe * rp)
{
	struct fnstate * vs, * fs;
	struct lav5_stage nvs, nfs;
	struct lav5_link nlink;
	struct lbuf vc = { 0 }, fc = { 0 };
	char why[256] = "";
	int ok;

	if (!rp->fs_deferred) {
		return YES;
	}
	vs = STATE(rp->vfn, struct fnstate);
	fs = STATE(rp->ffn, struct fnstate);
	memset(&nvs, 0, sizeof(nvs));
	memset(&nfs, 0, sizeof(nfs));
	memset(&nlink, 0, sizeof(nlink));
	pthread_mutex_lock(&gCCLock);
	ok = pipeCacheLoadSpec(vs, fs, NULL, 0, &nvs, &nfs, &nlink, rp->spec_bufs, &rp->spec_nbufs);
	if (!ok) {
		ok = lavcc_compile_program(STATE(vs->library, struct libstate)->lib, [vs->name UTF8String], vs->fcs, vs->nfcs,
		    STATE(fs->library, struct libstate)->lib, [fs->name UTF8String], fs->fcs, fs->nfcs,
		    &nvs, &nfs, &nlink, why, sizeof(why));
		if (ok) {
			pipeCacheStoreSpec(vs, fs, NULL, 0, &nvs, &nfs, &nlink, rp->spec_bufs, rp->spec_nbufs);
		}
	}
	pthread_mutex_unlock(&gCCLock);
	if (!ok) {
		lavlog("deferred base compile %s + %s failed: %s", [vs->name UTF8String], [fs->name UTF8String], why);
		return NO;
	}
	if (!codeBufLocked(&vc, &nvs.k) || !codeBufLocked(&fc, &nfs.k)) {
		if (vc.handle != 0) lavBufFree(&vc);
		if (fc.handle != 0) lavBufFree(&fc);
		lavcc_stage_free(&nvs);
		lavcc_stage_free(&nfs);
		return NO;
	}
	if (rp->vcode.handle != 0) lavBufFree(&rp->vcode);
	if (rp->fcode.handle != 0) lavBufFree(&rp->fcode);
	lavcc_stage_free(&rp->vs);
	lavcc_stage_free(&rp->fs);
	rp->vs = nvs;
	rp->fs = nfs;
	rp->link = nlink;
	rp->vcode = vc;
	rp->fcode = fc;
	rp->fs_deferred = NO;
	lavlog("deferred base compiled %s + %s: %u+%u words", [vs->name UTF8String], [fs->name UTF8String],
	    rp->vs.k.nwords, rp->fs.k.nwords);
	return YES;
}

static struct rpipe_spec *
pipeSelectSpec(struct rpipe * rp, const struct lav5_binding * fs_buf)
{
	static uint32_t diagLog;
	uint8_t keys[LAVCC_MAX_UBER_BUFS][LAVCC_UBER_BUF_CAP];
	struct lavcc_uber_val uvals[LAVCC_MAX_UBER_BUFS];
	uint32_t nuvals = 0;
	struct fnstate * vs, * fs;
	struct lav5_stage svs, sfs;
	struct lav5_link slink;
	struct lbuf vc = { 0 }, fc = { 0 };
	struct rpipe_spec * sp;
	uint64_t h;
	char why[256] = "";
	int ok;

	if (rp->spec_nbufs == 0 || rp->spec_nbufs > LAVCC_MAX_UBER_BUFS) {
		return NULL;
	}
	uint32_t nbound = 0;
	for (uint32_t k = 0; k < rp->spec_nbufs; k++) {
		const struct lavcc_uber_buf * ub = &rp->spec_bufs[k];
		const struct lav5_binding * b;
		uint32_t clen;

		if (ub->binding >= LAV5_MAX_BUFFERS || ub->len == 0 || ub->len > LAVCC_UBER_BUF_CAP) {
			continue;
		}
		b = &fs_buf[ub->binding];
		if (b->cpu != NULL && b->bytes > 0) {
			clen = ub->len;
			if (b->bytes < clen) {
				clen = (uint32_t)b->bytes;
			}
			for (uint32_t i = 0; i < clen; i++) {
				keys[nuvals][i] = ub->mask[i] ? ((const uint8_t *)b->cpu)[i] : 0;
			}
			nbound++;
		} else {
			/* Unbound UBO reads zeros from hole_gpu on the GPU; fold as zero so dead branches collapse. */
			clen = ub->len;
			memset(keys[nuvals], 0, clen);
		}
		uvals[nuvals].binding = ub->binding;
		uvals[nuvals].len = clen;
		uvals[nuvals].bytes = keys[nuvals];
		uvals[nuvals].fold = ub->mask;
		nuvals++;
	}
	if (nuvals == 0 || nbound == 0) {
		return NULL;
	}
	vs = STATE(rp->vfn, struct fnstate);
	fs = STATE(rp->ffn, struct fnstate);
	h = pipeKeyHash(vs, fs, uvals, nuvals);
	for (uint32_t i = 0; i < gSpecCount; i++) {
		if (gSpecCache[i].hash == h && gSpecCache[i].nbufs == nuvals) {
			BOOL match = YES;
			for (uint32_t k = 0; k < nuvals; k++) {
				if (gSpecCache[i].bindings[k] != uvals[k].binding || gSpecCache[i].lens[k] != uvals[k].len ||
				    memcmp(gSpecCache[i].keys[k], keys[k], uvals[k].len) != 0) {
					match = NO;
					break;
				}
			}
			if (match) {
				return gSpecCache[i].failed ? NULL : &gSpecCache[i];
			}
		}
	}
	if (gSpecCount >= RPIPE_GLOBAL_SPECS) {
		return NULL;
	}
	if (diagLog < 8) {
		char binfo[220] = "", sinfo[160] = "";
		size_t bp = 0, sp2 = 0;

		for (uint32_t i = 0; i < LAV5_MAX_BUFFERS && bp + 20 < sizeof(binfo); i++) {
			if (fs_buf[i].cpu != NULL) {
				bp += (size_t)snprintf(binfo + bp, sizeof(binfo) - bp, "%sb%u:%lluB",
				    bp ? " " : "", i, (unsigned long long)fs_buf[i].bytes);
			}
		}
		for (uint32_t k = 0; k < nuvals && sp2 + 32 < sizeof(sinfo); k++) {
			sp2 += (size_t)snprintf(sinfo + sp2, sizeof(sinfo) - sp2, "%sb%u:%uB[",
			    sp2 ? " " : "", uvals[k].binding, uvals[k].len);
			for (uint32_t j = 0; j < uvals[k].len && j < 8 && sp2 + 4 < sizeof(sinfo); j++) {
				sp2 += (size_t)snprintf(sinfo + sp2, sizeof(sinfo) - sp2, "%02x", keys[k][j]);
			}
			if (sp2 + 2 < sizeof(sinfo)) {
				sinfo[sp2++] = ']';
				sinfo[sp2] = 0;
			}
		}
		lavlog("spec try %s[%u]: bound=[%s] fold=[%s]", [fs->name UTF8String], gSpecCount, binfo, sinfo);
		diagLog++;
	}
	memset(&svs, 0, sizeof(svs));
	memset(&sfs, 0, sizeof(sfs));
	memset(&slink, 0, sizeof(slink));
	pthread_mutex_lock(&gCCLock);
	ok = pipeCacheLoadSpec(vs, fs, uvals, nuvals, &svs, &sfs, &slink, NULL, NULL);
	if (!ok) {
		ok = lavcc_compile_program_spec(STATE(vs->library, struct libstate)->lib, [vs->name UTF8String], vs->fcs, vs->nfcs,
		    STATE(fs->library, struct libstate)->lib, [fs->name UTF8String], fs->fcs, fs->nfcs,
		    uvals, nuvals, &svs, &sfs, &slink, why, sizeof(why));
		if (ok) {
			pipeCacheStoreSpec(vs, fs, uvals, nuvals, &svs, &sfs, &slink, NULL, 0);
		}
	}
	pthread_mutex_unlock(&gCCLock);
	sp = &gSpecCache[gSpecCount++];
	memset(sp, 0, sizeof(*sp));
	sp->hash = h;
	sp->nbufs = nuvals;
	for (uint32_t k = 0; k < nuvals; k++) {
		sp->bindings[k] = uvals[k].binding;
		sp->lens[k] = uvals[k].len;
		memcpy(sp->keys[k], keys[k], uvals[k].len);
	}
	if (!ok) {
		sp->failed = YES;
		lavlog("specialize %s + %s failed: %s", [vs->name UTF8String], [fs->name UTF8String], why);
		return NULL;
	}
	if (sfs.k.nwords >= 4096) {
		sp->failed = YES;
		lavlog("specialized %s[%u]: still %u words (base %u, %u bufs) -> rejected, using base",
		    [fs->name UTF8String], gSpecCount - 1, sfs.k.nwords, rp->fs.k.nwords, nuvals);
		lavcc_stage_free(&svs);
		lavcc_stage_free(&sfs);
		return NULL;
	}
	if (!codeBufLocked(&vc, &svs.k) || !codeBufLocked(&fc, &sfs.k)) {
		if (vc.handle != 0) lavBufFree(&vc);
		if (fc.handle != 0) lavBufFree(&fc);
		lavcc_stage_free(&svs);
		lavcc_stage_free(&sfs);
		sp->failed = YES;
		return NULL;
	}
	sp->vs = svs;
	sp->fs = sfs;
	sp->link = slink;
	sp->vcode = vc;
	sp->fcode = fc;
	lavlog("specialized %s[%u]: %u+%u words (base %u, %u bufs)", [fs->name UTF8String], gSpecCount - 1,
	    sp->vs.k.nwords, sp->fs.k.nwords, rp->fs.k.nwords, nuvals);
	return sp;
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

static struct lbuf gPvt[2];     /* private memory of the vertex and the fragment stage, grown on demand */

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
	if (!bindStage(r->vs_buf, r->vbuf, d->vbuf, d->voff, d->vbytes, d->vblen, rp->stride, &boff, name) ||
	    !bindStage(r->fs_buf, NULL, d->fbuf, d->foff, d->fbytes, d->fblen, NULL, &boff, name)) {
		free(r);
		return NO;
	}
	{
		struct rpipe_spec * sp = pipeSelectSpec(rp, r->fs_buf);

		if (sp != NULL) {
			r->vs = &sp->vs;
			r->fs = &sp->fs;
			r->link = &sp->link;
			r->vs_code = sp->vcode.gpu;
			r->fs_code = sp->fcode.gpu;
		} else {
			if (rp->fs_deferred && !pipeEnsureBaseLocked(rp)) {
				free(r);
				return NO;
			}
			r->vs = &rp->vs;
			r->fs = &rp->fs;
			r->link = &rp->link;
			r->vs_code = rp->vcode.gpu;
			r->fs_code = rp->fcode.gpu;
		}
	}
	for (int i = 0; i < 2; i++) {
		const struct lav5_stage * st = i == 0 ? r->vs : r->fs;
		uint32_t bytes = lav5_pvtmem_bytes(st);

		if (bytes == 0) {
			continue;
		}
		if (bytes > gPvt[i].len) {
			struct lbuf nb;

			if (!lavBufNew(bytes, &nb)) {
				lavlog("draw of %s: no memory for %u bytes of private memory", name, bytes);
				free(r);
				return NO;
			}
			if (gPvt[i].handle != 0) {
				lavBufFree(&gPvt[i]);
			}
			gPvt[i] = nb;
		}
		if (i == 0) {
			r->vs_pvt_gpu = gPvt[i].gpu;
		} else {
			r->fs_pvt_gpu = gPvt[i].gpu;
		}
	}
	r->rt_gpu = t->b.gpu;
	r->rt_width = t->width;
	r->rt_height = t->height;
	r->rt_pitch = t->pitch;
	r->rt_format = lavFmtFromMetal(t->format, NULL);
	r->nattr = rp->nattr;
	memcpy(r->attr, rp->attr, sizeof(r->attr));
	r->prim = d->prim;
	r->first = d->first;
	r->count = d->count;
	r->instances = d->instances;
	r->base_instance = d->base_instance;
	if (d->isize != 0 && d->ibuf != nil) {
		struct bufstate * ib = STATE(d->ibuf, struct bufstate);

		if (d->ioff >= ib->b.len) {
			lavlog("indexed draw: the index buffer offset is past its end");
			free(r);
			return NO;
		}
		r->index_gpu = ib->b.gpu + d->ioff;
		r->index_size = d->isize;
		r->index_max = (uint32_t)((ib->b.len - d->ioff) / (d->isize == 1 ? 2 : 4));
	}
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

			r->rt[i] = (struct lav5_rt){ xt->b.gpu, xt->pitch, lavFmtFromMetal(xt->format, NULL), 1 };
			r->nrt = i + 1;
		}
	}
	r->rt[0] = (struct lav5_rt){ r->rt_gpu, r->rt_pitch, r->rt_format, 1 };
	memcpy(r->blend_color, d->blend_color, sizeof(r->blend_color));
	for (int i = 0; i < 16; i++) {
		if (d->ftex[i] != nil) {
			struct texstate * ts = STATE(d->ftex[i], struct texstate);
			uint32_t tf = lavFmtFromMetal(ts->format, NULL);

			surfacePullIfDirty(ts);
			r->tex[i] = (struct lav5_tex){ ts->b.gpu, ts->width, ts->height, ts->pitch,
			    (ts->format == PF_BGRA8 || ts->format == PF_BGRA8_SRGB) ? 1u : 0u, 1u, tf + 1u };
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
	if (!ok) {
		lavlog("draw %s+%s (fs %u words, rt %ux%u fmt %u): %u vertices x %u instances, prim %u, %zu words, FAILED",
		    [STATE(rp->vfn, struct fnstate)->name UTF8String], [STATE(rp->ffn, struct fnstate)->name UTF8String],
		    r->fs->k.nwords, r->rt_width, r->rt_height, t->format, d->count, d->instances, d->prim, n);
	} else {
		lavdbg("draw: %u vertices x %u instances, prim %u, %zu words, done", d->count, d->instances, d->prim, n);
	}
	free(w);
	free(r);
	return ok;
}
