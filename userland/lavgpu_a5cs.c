/*
 * lavgpu_a5cs: M5.3 step (b). Runs ONE compute dispatch on the Adreno 512 through the LavenderGPU service: a kernel
 * compiled by Mesa's ir3 for gpu 512 (tools/mesa-host, from hakios's AIR front end), the PM4 stream made by
 * tools/a5cs/a5cs_gen.c from Mesa's A5xx register headers. The kernel writes out[i] = i * 0x9e3779b1 + seed for i in
 * 0..255; the program checks every word.
 *
 * Build (on the Mac):  xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios16.0 -O1 -I../tools/a5cs \
 *                        -framework IOKit -framework CoreFoundation -o lavgpu_a5cs lavgpu_a5cs.c && codesign -s - -f lavgpu_a5cs
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "a5cs_fill.h"

enum { kLavInfo = 0, kLavBufNew = 1, kLavBufFree = 2, kLavSubmit = 3, kLavSubmitBuf = 4 };

struct lav_buf_new { uint64_t size; uint32_t handle, pad; uint64_t address; };
struct lav_submit_buf { uint32_t handle, offset, dwords, pad; };
struct lav_submit_out { uint32_t fence, status; };

#define OUT_OFF   4096
#define STREAM_OFF 8192

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
	int wrong = 0;

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
	uint64_t base[2] = { bn.address, bn.address + OUT_OFF };
	uint32_t stream[sizeof(a5cs_stream) / 4];

	memcpy(stream, a5cs_stream, sizeof(stream));
	for (size_t i = 0; i < sizeof(a5cs_reloc) / sizeof(a5cs_reloc[0]); i++) {
		uint64_t a = base[a5cs_reloc[i].buf] + a5cs_reloc[i].off;

		stream[a5cs_reloc[i].at] = (uint32_t)a;
		stream[a5cs_reloc[i].at + 1] = (uint32_t)(a >> 32);
	}
	for (int i = 0; i < 64; i++) {
		mem[i] = a5cs_code[i];
	}
	for (int i = 0; i < A5CS_LOCAL * A5CS_GROUPS; i++) {
		mem[OUT_OFF / 4 + i] = 0xdeadbeefu;
	}
	for (size_t i = 0; i < sizeof(stream) / 4; i++) {
		mem[STREAM_OFF / 4 + i] = stream[i];
	}

	struct lav_submit_buf sb = { bn.handle, STREAM_OFF, (uint32_t)(sizeof(stream) / 4), 0 };
	size_t out_size = sizeof(out);

	memset(&out, 0, sizeof(out));
	printf("submitting %u dwords\n", sb.dwords);
	kr = IOConnectCallStructMethod(conn, kLavSubmitBuf, &sb, sizeof(sb), &out, &out_size);
	printf("submit: kr %#x fence %u status %u\n", kr, out.fence, out.status);

	for (int i = 0; i < A5CS_LOCAL * A5CS_GROUPS; i++) {
		uint32_t want = (uint32_t)i * 0x9e3779b1u + A5CS_SEED;

		if (mem[OUT_OFF / 4 + i] != want) {
			if (wrong < 8) {
				printf("  out[%d] = %08x, wanted %08x\n", i, mem[OUT_OFF / 4 + i], want);
			}
			wrong++;
		}
	}
	printf("%d of %d words wrong: %s\n", wrong, A5CS_LOCAL * A5CS_GROUPS, wrong == 0 ? "PASSED" : "FAILED");
	IOConnectCallStructMethod(conn, kLavBufFree, &(uint32_t[2]){ bn.handle, 0 }, 8, NULL, NULL);
	IOServiceClose(conn);
	return wrong != 0;
}
