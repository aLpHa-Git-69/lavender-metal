/*
 * lavgpu_test: a program talking to the LavenderGPU IOKit service (kernel/LavenderGPU.cpp).
 *
 *   lavgpu_test            open the service, ask for its state, make a buffer, map it, have the GPU write two words
 *                          into it with a submitted PM4 stream, read them back, then do the same with a stream
 *                          longer than 4 KB (which IOKit hands over as a memory descriptor), and give the buffer back.
 *
 * Build (on the Mac):  xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios16.0 -O1 \
 *                        -framework IOKit -framework CoreFoundation -o lavgpu_test lavgpu_test.c && codesign -s - lavgpu_test
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kLavInfo = 0, kLavBufNew = 1, kLavBufFree = 2, kLavSubmit = 3, kLavSubmitBuf = 4 };

struct lav_info { uint32_t version, ready, submits, last_fence; };
struct lav_buf_new { uint64_t size; uint32_t handle, pad; uint64_t address; };
struct lav_buf_free { uint32_t handle, pad; };
struct lav_submit_out { uint32_t fence, status; };
struct lav_submit_buf { uint32_t handle, offset, dwords, pad; };

static uint32_t
parity(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	return (0x9669u >> (v & 0xf)) & 1u;
}

static uint32_t
pkt7(uint32_t opcode, uint32_t count)
{
	return 0x70000000u | (count & 0x3fff) | (parity(count) << 15) | ((opcode & 0x7f) << 16) | (parity(opcode) << 23);
}

static int
submit(io_connect_t conn, const uint32_t *words, size_t count, struct lav_submit_out *out)
{
	size_t out_size = sizeof(*out);
	kern_return_t kr = IOConnectCallStructMethod(conn, kLavSubmit, words, count * 4, out, &out_size);

	printf("  submit of %zu bytes: kr %#x fence %u status %u\n", count * 4, kr, out->fence, out->status);
	return kr == KERN_SUCCESS && out->fence != 0;
}

int
main(int argc, char **argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
	io_connect_t conn = 0;
	struct lav_info info;
	struct lav_buf_new bn;
	struct lav_buf_free bf;
	struct lav_submit_out out;
	size_t size;
	mach_vm_address_t map = 0;
	mach_vm_size_t map_size = 0;
	volatile uint32_t *words;
	uint32_t *stream;
	kern_return_t kr;
	int failed = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (service == 0) {
		printf("no LavenderGPU service\n");
		return 1;
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	printf("IOServiceOpen: kr %#x\n", kr);
	if (kr != KERN_SUCCESS) {
		return 1;
	}

	memset(&info, 0, sizeof(info));
	size = sizeof(info);
	kr = IOConnectCallStructMethod(conn, kLavInfo, NULL, 0, &info, &size);
	printf("info: kr %#x version %u ready %u submits %u last fence %u\n", kr, info.version, info.ready, info.submits,
	    info.last_fence);

	memset(&bn, 0, sizeof(bn));
	bn.size = 65536;
	size = sizeof(bn);
	kr = IOConnectCallStructMethod(conn, kLavBufNew, &bn, sizeof(bn), &bn, &size);
	printf("buffer: kr %#x handle %u gpu address %#llx size %llu\n", kr, bn.handle, bn.address, bn.size);
	if (kr != KERN_SUCCESS) {
		return 1;
	}
	kr = IOConnectMapMemory64(conn, bn.handle, mach_task_self(), &map, &map_size, kIOMapAnywhere);
	printf("map: kr %#x at %#llx, %llu bytes\n", kr, (unsigned long long)map, (unsigned long long)map_size);
	if (kr != KERN_SUCCESS) {
		return 1;
	}
	words = (volatile uint32_t *)(uintptr_t)map;
	printf("  the program sees the buffer zeroed: %08x %08x\n", words[0], words[1]);

	/* 1: two words written by the GPU (CP_MEM_WRITE: address, then the words) */
	uint32_t small[8] = {
		pkt7(61, 3), (uint32_t)bn.address, (uint32_t)(bn.address >> 32), 0x5eed1234u,
		pkt7(61, 3), (uint32_t)(bn.address + 4), (uint32_t)((bn.address + 4) >> 32), 0x600dcafeu,
	};
	printf("test 1: a short stream\n");
	if (!submit(conn, small, 8, &out)) {
		failed++;
	}
	printf("  words now %08x %08x (want 5eed1234 600dcafe) %s\n", words[0], words[1],
	    (words[0] == 0x5eed1234u && words[1] == 0x600dcafeu) ? "OK" : "WRONG");
	if (words[0] != 0x5eed1234u || words[1] != 0x600dcafeu) {
		failed++;
	}

	/* 2: a long stream (6 KB) placed in the buffer itself and submitted by reference: a CP_NOP that swallows
	 * 1500 words, then a write. IOKit's large-input path is only tried when asked for with an argument. */
	size_t n = 1 + 1500 + 4;
	uint32_t *buf32 = (uint32_t *)(uintptr_t)map;
	uint32_t *st = buf32 + 1024;			/* byte offset 4096 */

	memset(st, 0, n * 4);
	st[0] = pkt7(16, 1500);
	st[1501] = pkt7(61, 3);
	st[1502] = (uint32_t)(bn.address + 64);
	st[1503] = (uint32_t)((bn.address + 64) >> 32);
	st[1504] = 0xabcd0042u;
	printf("test 2: a stream of %zu bytes, submitted from the buffer\n", n * 4);
	{
		struct lav_submit_buf sb = { bn.handle, 4096, (uint32_t)n, 0 };
		size_t out_size = sizeof(out);

		memset(&out, 0, sizeof(out));
		kr = IOConnectCallStructMethod(conn, kLavSubmitBuf, &sb, sizeof(sb), &out, &out_size);
		printf("  submit by reference: kr %#x fence %u status %u\n", kr, out.fence, out.status);
		if (kr != KERN_SUCCESS || out.fence == 0) {
			failed++;
		}
	}
	printf("  word at +64: %08x (want abcd0042) %s\n", words[16], words[16] == 0xabcd0042u ? "OK" : "WRONG");
	if (words[16] != 0xabcd0042u) {
		failed++;
	}

	if (argc > 1) {
		/* 3: the same stream through the inline/large-input path (this one hung the phone once) */
		stream = calloc(n, 4);
		stream[0] = pkt7(16, 1500);
		stream[1501] = pkt7(61, 3);
		stream[1502] = (uint32_t)(bn.address + 128);
		stream[1503] = (uint32_t)((bn.address + 128) >> 32);
		stream[1504] = 0xfeed0099u;
		printf("test 3: a stream of %zu bytes as a struct input\n", n * 4);
		if (!submit(conn, stream, n, &out)) {
			failed++;
		}
		printf("  word at +128: %08x (want feed0099)\n", words[32]);
		free(stream);
	}

	memset(&bf, 0, sizeof(bf));
	bf.handle = bn.handle;
	kr = IOConnectCallStructMethod(conn, kLavBufFree, &bf, sizeof(bf), NULL, NULL);
	printf("free: kr %#x\n", kr);

	memset(&info, 0, sizeof(info));
	size = sizeof(info);
	kr = IOConnectCallStructMethod(conn, kLavInfo, NULL, 0, &info, &size);
	printf("info: kr %#x submits %u last fence %u\n", kr, info.submits, info.last_fence);
	IOServiceClose(conn);
	printf("%s\n", failed ? "FAILED" : "PASSED");
	return failed != 0;
}
