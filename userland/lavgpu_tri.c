/*
 * lavgpu_tri: M5.4 step (a). One triangle on the Adreno 512 through the LavenderGPU service, with no Metal API in between: the
 * vertex and fragment functions of lavk.metallib compiled by Mesa's ir3 (lavcc), the PM4 stream of lava5r.c, a 64x64 BGRA
 * target in memory. Two draws: tri_vs (the vertex id alone) and attr_vs (a vertex buffer). The corners of the triangle are
 * red, green and blue, so the pixels near them say whether the position, the winding and the interpolation are right.
 *
 *   lavgpu_tri lavk.metallib
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lava5.h"
#include "lavcc.h"

enum { kLavBufNew = 1, kLavSubmitBuf = 4 };

struct lav_buf_new { uint64_t size; uint32_t handle, pad; uint64_t address; };
struct lav_submit_buf { uint32_t handle, offset, dwords, pad; };
struct lav_submit_out { uint32_t fence, status; };

struct buf {
	uint32_t handle;
	uint64_t gpu;
	uint8_t * cpu;
};

#define W 64
#define H 64
#define CLEAR 0xff202020u

static io_connect_t conn;

static int
newbuf(struct buf * b, size_t size)
{
	struct lav_buf_new bn;
	size_t out = sizeof(bn);
	mach_vm_address_t map = 0;
	mach_vm_size_t msize = 0;

	memset(&bn, 0, sizeof(bn));
	bn.size = size;
	if (IOConnectCallStructMethod(conn, kLavBufNew, &bn, sizeof(bn), &bn, &out) != KERN_SUCCESS) {
		return 0;
	}
	if (IOConnectMapMemory64(conn, bn.handle, mach_task_self(), &map, &msize, kIOMapAnywhere) != KERN_SUCCESS) {
		return 0;
	}
	b->handle = bn.handle;
	b->gpu = bn.address;
	b->cpu = (uint8_t *)(uintptr_t)map;
	memset(b->cpu, 0, size);
	return 1;
}

static uint32_t
px(const struct buf * rt, int x, int y)
{
	return ((uint32_t *)rt->cpu)[y * W + x];
}

static void
putcode(struct buf * b, size_t off, const struct lav5_kernel * k)
{
	memset(b->cpu + off, 0, (size_t)k->instrlen * 128);
	memcpy(b->cpu + off, k->words, (size_t)k->nwords * 4);
}

/* B, G, R of a BGRA pixel */
#define CB(p) ((p) & 0xff)
#define CG(p) (((p) >> 8) & 0xff)
#define CR(p) (((p) >> 16) & 0xff)

static void
dump(const char * n, const struct lav5_stage * s)
{
	printf("  %s: vid r%u.%u inst r%u pos r%u.%u psize %#x dp_vec4 %d imm_vec4 %u nimm %u nranges %u constlen %u instrlen %u maxreg %d half %d dbl %u bs %u ssbo %u ubos %u mask %#x other %u\n",
	    n, s->vertex_id_regid >> 2, s->vertex_id_regid & 3, s->instance_id_regid, s->position_regid >> 2, s->position_regid & 3,
	    s->point_size_regid, s->k.dp_vec4, s->k.imm_vec4, s->k.nimm, s->k.nranges, s->k.constlen, s->k.instrlen, s->k.max_reg,
	    s->k.max_half_reg, s->k.double_threadsize, s->k.branchstack, s->k.num_ssbos, s->k.num_ubos, s->k.buffer_mask, s->k.other_resources);
	for (uint32_t i = 0; i < s->nin; i++) {
		printf("    in[%u]: slot %u regid %u mask %#x inloc %u sysval %u bary %u flat %u\n", i, s->in[i].slot, s->in[i].regid,
		    s->in[i].compmask, s->in[i].inloc, s->in[i].sysval, s->in[i].bary, s->in[i].flat);
	}
	for (uint32_t i = 0; i < s->nout; i++) {
		printf("    out[%u]: slot %u regid %u\n", i, s->out[i].slot, s->out[i].regid);
	}
	for (uint32_t i = 0; i < s->k.nimm; i++) {
		printf("    imm[%u] = %08x\n", i, s->k.imm[i]);
	}
	for (uint32_t i = 0; i < s->k.nranges; i++) {
		printf("    range: block %u vec4 %u start %u size %u\n", s->k.ranges[i].block, s->k.ranges[i].offset_vec4, s->k.ranges[i].start_dword,
		    s->k.ranges[i].size_dwords);
	}
	if (s->is_fragment) {
		printf("    fs: color r%u.%u ncolor %u ij %u %u %u %u %u %u %u coord %u face %u total_in %u\n", s->color_regid[0] >> 2, s->color_regid[0] & 3,
		    s->ncolor, s->ij_regid[0], s->ij_regid[1], s->ij_regid[2], s->ij_regid[3], s->ij_regid[4], s->ij_regid[5], s->ij_regid[6],
		    s->frag_coord_regid, s->face_regid, s->total_in);
	}
}

static void
dumplink(const struct lav5_link * l)
{
	printf("  link: cnt %u max_loc %u varmask %08x position_loc %u psize_loc %u interp0 %08x\n", l->cnt, l->max_loc, l->varmask[0],
	    l->position_loc, l->psize_loc, l->interp[0]);
	for (uint32_t i = 0; i < l->cnt; i++) {
		printf("    var[%u]: slot %u regid %u mask %#x loc %u\n", i, l->var[i].slot, l->var[i].regid, l->var[i].compmask, l->var[i].loc);
	}
}

static int
draw(const char * name, struct lav5_stage * vs, struct lav5_stage * fs, struct lav5_link * link, int attributes)
{
	struct buf stream, res;
	struct lav5_render r;
	struct lav_submit_buf sb;
	struct lav_submit_out so = { 0, 0 };
	size_t so_size = sizeof(so), n;
	const char * why = NULL;
	uint32_t * w = malloc(4096 * 4);
	int ok = 1;
	float verts[3][6] = {
		{ -0.8f, -0.8f, 1, 0, 0, 1 },
		{  0.8f, -0.8f, 0, 1, 0, 1 },
		{  0.0f,  0.8f, 0, 0, 1, 1 },
	};
	uint32_t * rt;

	printf("%s\n", name);
	dump("vs", vs);
	dump("fs", fs);
	dumplink(link);
	if (!newbuf(&stream, 65536) || !newbuf(&res, 65536)) {
		printf("  buffers: failed\n");
		return 0;
	}
	/* res: VS code at 0, FS code at 4096, vertices at 8192, the target at 16384 */
	putcode(&res, 0, &vs->k);
	putcode(&res, 4096, &fs->k);
	memcpy(res.cpu + 8192, verts, sizeof(verts));
	rt = (uint32_t *)(res.cpu + 16384);
	for (int i = 0; i < W * H; i++) {
		rt[i] = CLEAR;
	}

	memset(&r, 0, sizeof(r));
	r.vs = vs;
	r.fs = fs;
	r.link = link;
	r.vs_code = res.gpu;
	r.fs_code = res.gpu + 4096;
	r.rt_gpu = res.gpu + 16384;
	r.rt_width = W;
	r.rt_height = H;
	r.rt_pitch = W * 4;
	r.rt_format = 0;
	if (attributes) {
		r.nattr = 2;
		r.attr[0] = (struct lav5_attr){ 0, 103 /* 32_32_FLOAT */, 0, 0, 1, 0 };
		r.attr[1] = (struct lav5_attr){ 1, 130 /* 32_32_32_32_FLOAT */, 8, 0, 1, 0 };
		r.vbuf[0].gpu = res.gpu + 8192;
		r.vbuf[0].bytes = sizeof(verts);
		r.vbuf[0].stride = 24;
	}
	r.prim = 4;                 /* DI_PT_TRILIST */
	r.first = 0;
	r.count = 3;
	r.instances = 1;
	r.viewport[0] = 0; r.viewport[1] = 0; r.viewport[2] = W; r.viewport[3] = H; r.viewport[4] = 0; r.viewport[5] = 1;
	r.scissor[0] = 0; r.scissor[1] = 0; r.scissor[2] = W; r.scissor[3] = H;
	r.cull = 0;
	r.blend.writemask = 0xf;
	r.front_cw = 1;
	r.flush_gpu = stream.gpu + 0x8000 + 64;
	((uint32_t *)(stream.cpu + 0x8000 + 64))[0] = 0;
	((uint32_t *)(stream.cpu + 0x8000 + 64))[1] = 0;

	n = lav5_build_render(w, 4096, &r, &why);
	if (n == 0) {
		printf("  the builder refused: %s\n", why ? why : "?");
		free(w);
		return 0;
	}
	printf("  stream of %zu words\n", n);
	memcpy(stream.cpu, w, n * 4);
	free(w);
	sb.handle = stream.handle;
	sb.offset = 0;
	sb.dwords = (uint32_t)n;
	sb.pad = 0;
	kern_return_t kr = IOConnectCallStructMethod(conn, kLavSubmitBuf, &sb, sizeof(sb), &so, &so_size);
	printf("  submit: kr %#x fence %u status %u, flush words %08x %08x\n", kr, so.fence, so.status,
	    ((uint32_t *)(stream.cpu + 0x8000 + 64))[0], ((uint32_t *)(stream.cpu + 0x8000 + 64))[1]);
	if (kr != KERN_SUCCESS || so.fence == 0) {
		return 0;
	}
	/* the target is at res + 16384: px() above reads from the start of res, so read it properly */
	{
		const struct buf t = { 0, 0, res.cpu + 16384 };
		uint32_t a = px(&t, 1, 1), c = px(&t, 32, 40), rr = px(&t, 10, 55), gg = px(&t, 54, 55), bb = px(&t, 32, 10);
		int covered = 0;

		for (int i = 0; i < W * H; i++) {
			covered += ((uint32_t *)t.cpu)[i] != CLEAR;
		}
		printf("  target pixels: (1,1) %08x  (32,40) %08x  (10,55) %08x  (54,55) %08x  (32,10) %08x; %d of %d written\n", a, c, rr,
		    gg, bb, covered, W * H);
		ok &= a == CLEAR;
		ok &= c != CLEAR;
		ok &= CR(rr) > CG(rr) && CR(rr) > CB(rr);
		ok &= CG(gg) > CR(gg) && CG(gg) > CB(gg);
		ok &= CB(bb) > CR(bb) && CB(bb) > CG(bb);
		ok &= covered > 500 && covered < 2500;
	}
	printf("  %s\n", ok ? "PASSED" : "WRONG");
	return ok;
}

int
main(int argc, char ** argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
	FILE * f;
	uint8_t * data;
	long size;
	char why[256];
	struct lavcc_lib * lib;
	struct lav5_stage vs, fs, avs;
	struct lav5_link link, alink;
	int passed = 0, total = 0;

	if (argc < 2 || service == 0 || IOServiceOpen(service, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		printf("usage: lavgpu_tri lavk.metallib (and the LavenderGPU service)\n");
		return 1;
	}
	f = fopen(argv[1], "rb");
	if (f == NULL) {
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(size);
	if (fread(data, 1, size, f) != (size_t)size) {
		return 1;
	}
	fclose(f);
	lib = lavcc_open(data, size, why, sizeof(why));
	if (lib == NULL) {
		printf("library: %s\n", why);
		return 1;
	}
	if (!lavcc_compile_program(lib, "tri_vs", NULL, 0, lib, "tri_fs", NULL, 0, &vs, &fs, &link, why, sizeof(why))) {
		printf("tri_vs + tri_fs: the compiler refused: %s\n", why);
		return 1;
	}
	printf("tri_vs: %u words, instrlen %u, constlen %u, %u attributes; tri_fs: %u words, instrlen %u, constlen %u, total_in %u; link: %u entries, max_loc %u\n",
	    vs.k.nwords, vs.k.instrlen, vs.k.constlen, vs.nattr, fs.k.nwords, fs.k.instrlen, fs.k.constlen, fs.total_in, link.cnt,
	    link.max_loc);
	total++;
	passed += draw("draw 1: tri_vs (vertex id), tri_fs", &vs, &fs, &link, 0);
	if (lavcc_compile_program(lib, "attr_vs", NULL, 0, lib, "tri_fs", NULL, 0, &avs, &fs, &alink, why, sizeof(why))) {
		printf("attr_vs: %u words, %u attributes\n", avs.k.nwords, avs.nattr);
		total++;
		passed += draw("draw 2: attr_vs (a vertex buffer), tri_fs", &avs, &fs, &alink, 1);
	} else {
		printf("attr_vs + tri_fs: the compiler refused: %s\n", why);
	}
	printf("%d of %d draws passed\n", passed, total);
	return passed == total ? 0 : 1;
}
