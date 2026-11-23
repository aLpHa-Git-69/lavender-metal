/*
 * lavgpu_a5cs: M5.3 step (b). Runs ONE compute dispatch on the Adreno 512 through the LavenderGPU service: a kernel
 * compiled by Mesa's ir3 for gpu 512 (tools/mesa-host, from hakios's AIR front end), the PM4 stream made by
 * tools/a5cs/a5cs_gen.c from Mesa's A5xx register headers. The kernel writes out[i] = i * 0x9e3779b1 + seed for i in
 * 0..255; the program checks every word. Four variants of the stream (the code inline or from memory, a cache flush
 * with a timestamp at the end or not) are tried in turn, each after the output was poisoned.
 *
 * Build (on the Mac):  xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios16.0 -O1 -I../tools/a5cs \
 *                        -framework IOKit -framework CoreFoundation -o lavgpu_a5cs lavgpu_a5cs.c && codesign -s - -f lavgpu_a5cs
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "a5cs_v0.h"
#include "a5cs_v1.h"
#include "a5cs_v2.h"
#include "a5cs_v3.h"

enum { kLavInfo = 0, kLavBufNew = 1, kLavBufFree = 2, kLavSubmit = 3, kLavSubmitBuf = 4 };

struct lav_buf_new { uint64_t size; uint32_t handle, pad; uint64_t address; };
struct lav_submit_buf { uint32_t handle, offset, dwords, pad; };
struct lav_submit_out { uint32_t fence, status; };

#define OUT_OFF    4096
#define MARK_OFF   2048
#define STREAM_OFF 8192

struct variant {
	const char *name;
	const uint32_t *stream;
	size_t dwords;
	const struct a5cs_rel *reloc;
	size_t nreloc;
};

#define V(n, s) { s, a5cs_stream_v##n, sizeof(a5cs_stream_v##n) / 4, a5cs_reloc_v##n, sizeof(a5cs_reloc_v##n) / sizeof(a5cs_reloc_v##n[0]) }

static const struct variant variants[] = {
	V(0, "code inline, no timestamp flush"),
	V(1, "code inline, then a cache flush with a timestamp"),
	V(2, "code from memory, no timestamp flush"),
	V(3, "code from memory, then a cache flush with a timestamp"),
};

int
main(void)
{
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
	io_connect_t conn = 0;
	struct lav_buf_new bn;
	struct lav_submit_out out;
	size_t size = sizeof(bn);
	mach_vm_address_t map = 0;
	mach_vm_size_t map_size = 0;
	kern_return_t kr;
	int passed = 0;

	if (service == 0) {
		printf("no LavenderGPU service\n");
		return 1;
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	if (kr != KERN_SUCCESS) {
		printf("IOServiceOpen: kr %#x\n", kr);
		return 1;
	}
	memset(&bn, 0, sizeof(bn));
	bn.size = 65536;
	kr = IOConnectCallStructMethod(conn, kLavBufNew, &bn, sizeof(bn), &bn, &size);
	if (kr != KERN_SUCCESS) {
		printf("buffer: kr %#x\n", kr);
		return 1;
	}
	kr = IOConnectMapMemory64(conn, bn.handle, mach_task_self(), &map, &map_size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		printf("map: kr %#x\n", kr);
		return 1;
	}
	printf("buffer at gpu %#llx\n", bn.address);

	volatile uint32_t *mem = (volatile uint32_t *)(uintptr_t)map;
	uint64_t base[3] = { bn.address, bn.address + OUT_OFF, bn.address + MARK_OFF };

	for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
		const struct variant *var = &variants[v];
		uint32_t stream[512];
		int wrong = 0, first = -1;

		memcpy(stream, var->stream, var->dwords * 4);
		for (size_t i = 0; i < var->nreloc; i++) {
			uint64_t a = base[var->reloc[i].buf] + var->reloc[i].off;

			stream[var->reloc[i].at] |= (uint32_t)a;
			stream[var->reloc[i].at + 1] = (uint32_t)(a >> 32);
		}
		for (int i = 0; i < 64; i++) {
			mem[i] = a5cs_code[i];
		}
		for (int i = 0; i < 8; i++) {
			mem[MARK_OFF / 4 + i] = 0;
		}
		for (int i = 0; i < A5CS_LOCAL * A5CS_GROUPS; i++) {
			mem[OUT_OFF / 4 + i] = 0xdeadbeefu;
		}
		for (size_t i = 0; i < var->dwords; i++) {
			mem[STREAM_OFF / 4 + i] = stream[i];
		}

		struct lav_submit_buf sb = { bn.handle, STREAM_OFF, (uint32_t)var->dwords, 0 };
		size_t out_size = sizeof(out);

		memset(&out, 0, sizeof(out));
		kr = IOConnectCallStructMethod(conn, kLavSubmitBuf, &sb, sizeof(sb), &out, &out_size);
		printf("variant %zu (%s): %u dwords, kr %#x fence %u status %u\n", v, var->name, sb.dwords, kr, out.fence, out.status);
		printf("  markers %08x %08x %08x %08x, timestamp %08x\n", mem[MARK_OFF / 4], mem[MARK_OFF / 4 + 1],
		    mem[MARK_OFF / 4 + 2], mem[MARK_OFF / 4 + 3], mem[MARK_OFF / 4 + 4]);
		for (int i = 0; i < A5CS_LOCAL * A5CS_GROUPS; i++) {
			uint32_t want = (uint32_t)i * 0x9e3779b1u + A5CS_SEED;

			if (mem[OUT_OFF / 4 + i] != want) {
				if (first < 0) {
					first = i;
				}
				wrong++;
			}
		}
		printf("  %d of %d words wrong", wrong, A5CS_LOCAL * A5CS_GROUPS);
		if (wrong != 0) {
			printf(" (first %d: %08x)\n", first, mem[OUT_OFF / 4 + first]);
		} else {
			printf(": PASSED\n");
			passed++;
		}
	}
	IOConnectCallStructMethod(conn, kLavBufFree, &(uint32_t[2]){ bn.handle, 0 }, 8, NULL, NULL);
	IOServiceClose(conn);
	printf("%d of %zu variants passed\n", passed, sizeof(variants) / sizeof(variants[0]));
	return passed == 0;
}
