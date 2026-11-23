/*
 * lavmetal_test: asks the LavenderGPU service to name the Metal plug-in on IOAcceleratorES, then uses Metal the way a program
 * does: the device, buffers, a command queue and command buffers with a blit encoder (fill and copy), and checks every byte.
 * The plug-in's own log is /var/root/lavmetal.log.
 *
 *   lavmetal_test        the whole check (M5.1 + M5.2), stage by stage, stopping at the first failure
 *   lavmetal_test lavk.metallib   the same, then the compute stages (M5.3 d) with that library (userland/kernels)
 *   lavmetal_test off    only take the plug-in properties away (a crashed run leaves them set)
 *
 * Build (on the Mac): userland/build-lavmetal.sh
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurfaceRef.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurfaceRef.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurfaceRef.h>
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

		/* M5.3 (d): the compute side, with the library lavk.metallib (userland/kernels) */
		if (gFailed == 0 && argc > 1 && strcmp(argv[1], "nocompute") != 0) {
			NSError * err = nil;
			NSData * data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
			id<MTLLibrary> lib = nil;

			printf("stage 8: a library from %s (%lu bytes)\n", argv[1], (unsigned long)[data length]);
			if (data != nil) {
				dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);

				lib = [device newLibraryWithData:dd error:&err];
			}
			check(lib != nil, "newLibraryWithData gave a library");
			if (lib == nil && err != nil) {
				printf("    %s\n", [[err localizedDescription] UTF8String]);
			}
			id<MTLFunction> fFill = [lib newFunctionWithName:@"fill_id"];
			id<MTLFunction> fSaxpy = [lib newFunctionWithName:@"saxpy"];
			id<MTLFunction> fSquare = [lib newFunctionWithName:@"square"];
			check(fFill != nil && fSaxpy != nil && fSquare != nil, "newFunctionWithName: fill_id, saxpy, square");
			id<MTLComputePipelineState> pFill = nil, pSaxpy = nil, pSquare = nil;
			if (gFailed == 0) {
				pFill = [device newComputePipelineStateWithFunction:fFill error:&err];
				pSaxpy = [device newComputePipelineStateWithFunction:fSaxpy error:&err];
				pSquare = [device newComputePipelineStateWithFunction:fSquare error:&err];
				check(pFill != nil && pSaxpy != nil && pSquare != nil, "a compute pipeline state of each");
				if (pFill != nil) {
					printf("  fill_id: threadExecutionWidth %lu, maxTotalThreadsPerThreadgroup %lu\n",
					    (unsigned long)[pFill threadExecutionWidth], (unsigned long)[pFill maxTotalThreadsPerThreadgroup]);
				}
			}
			if (gFailed == 0) {
				printf("stage 9: fill_id, 1024 threads, the seed by setBytes\n");
				id<MTLBuffer> out = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
				uint32_t seed = 0x1234;
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
				[ce setComputePipelineState:pFill];
				[ce setBuffer:out offset:0 atIndex:0];
				[ce setBytes:&seed length:4 atIndex:1];
				[ce dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[ce endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
				uint32_t * o = [out contents];
				unsigned wrong = 0;
				for (uint32_t i = 0; i < 1024; i++) {
					if (o[i] != i * 0x9e3779b1u + seed) {
						if (wrong++ < 3) {
							printf("    word %u: %08x, wanted %08x\n", i, o[i], i * 0x9e3779b1u + seed);
						}
					}
				}
				check(wrong == 0, "all 1024 words are i * 0x9e3779b1 + seed");
			}
			if (gFailed == 0) {
				printf("stage 10: saxpy by dispatchThreads, y = 2 x + y over 512 floats\n");
				id<MTLBuffer> x = [device newBufferWithLength:2048 options:MTLResourceStorageModeShared];
				id<MTLBuffer> y = [device newBufferWithLength:2048 options:MTLResourceStorageModeShared];
				float * xs = [x contents], * ys = [y contents];
				float a = 2.0f;
				for (int i = 0; i < 512; i++) {
					xs[i] = (float)i;
					ys[i] = 1.0f;
				}
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
				[ce setComputePipelineState:pSaxpy];
				[ce setBuffer:x offset:0 atIndex:0];
				[ce setBuffer:y offset:0 atIndex:1];
				[ce setBytes:&a length:4 atIndex:2];
				[ce dispatchThreads:MTLSizeMake(512, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[ce endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
				unsigned wrong = 0;
				for (int i = 0; i < 512; i++) {
					if (ys[i] != 2.0f * (float)i + 1.0f && wrong++ < 3) {
						printf("    y[%d] = %f, wanted %f\n", i, ys[i], 2.0f * (float)i + 1.0f);
					}
				}
				check(wrong == 0, "all 512 floats are 2 i + 1");
			}
			if (gFailed == 0) {
				printf("stage 11: two dispatches in one command buffer: square, then square of the result (i^4)\n");
				id<MTLBuffer> in = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
				id<MTLBuffer> mid = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
				id<MTLBuffer> fin = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
				uint32_t * is = [in contents];
				for (uint32_t i = 0; i < 256; i++) {
					is[i] = i + 3;
				}
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
				[ce setComputePipelineState:pSquare];
				[ce setBuffer:in offset:0 atIndex:0];
				[ce setBuffer:mid offset:0 atIndex:1];
				[ce dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[ce setBuffer:mid offset:0 atIndex:0];
				[ce setBuffer:fin offset:0 atIndex:1];
				[ce dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[ce endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
				uint32_t * fs = [fin contents];
				unsigned wrong = 0;
				for (uint32_t i = 0; i < 256; i++) {
					uint32_t v = (i + 3) * (i + 3);
					if (fs[i] != v * v && wrong++ < 3) {
						printf("    word %u: %08x, wanted %08x\n", i, fs[i], v * v);
					}
				}
				check(wrong == 0, "all 256 words are (i + 3)^4");
			}
			/* M5.4: a render pass into a texture, the pipeline from a vertex and a fragment function */
			for (int variant = 0; variant < 2 && gFailed == 0; variant++) {
				printf("stage %d: render a triangle into a 64x64 BGRA texture with %s\n", 12 + variant,
				    variant == 0 ? "tri_vs (the vertex id)" : "attr_vs (a vertex buffer through a vertex descriptor)");
				id<MTLFunction> vf = [lib newFunctionWithName:variant == 0 ? @"tri_vs" : @"attr_vs"];
				id<MTLFunction> ff = [lib newFunctionWithName:@"tri_fs"];
				check(vf != nil && ff != nil, "the vertex and fragment functions");
				MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
				rpd.vertexFunction = vf;
				rpd.fragmentFunction = ff;
				rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				if (variant == 1) {
					MTLVertexDescriptor * vd = [MTLVertexDescriptor vertexDescriptor];
					vd.attributes[0].format = MTLVertexFormatFloat2;
					vd.attributes[0].offset = 0;
					vd.attributes[0].bufferIndex = 0;
					vd.attributes[1].format = MTLVertexFormatFloat4;
					vd.attributes[1].offset = 8;
					vd.attributes[1].bufferIndex = 0;
					vd.layouts[0].stride = 24;
					rpd.vertexDescriptor = vd;
				}
				id<MTLRenderPipelineState> rps = [device newRenderPipelineStateWithDescriptor:rpd error:&err];
				check(rps != nil, "a render pipeline state");
				if (rps == nil && err != nil) {
					printf("    %s\n", [[err localizedDescription] UTF8String]);
				}
				MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				    width:64 height:64 mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				td.storageMode = MTLStorageModeShared;
				id<MTLTexture> tex = [device newTextureWithDescriptor:td];
				check(tex != nil, "a 64x64 texture");
				if (gFailed == 0) {
					static const float verts[3][6] = {
						{ -0.8f, -0.8f, 1, 0, 0, 1 }, { 0.8f, -0.8f, 0, 1, 0, 1 }, { 0.0f, 0.8f, 0, 0, 1, 1 },
					};
					MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
					pass.colorAttachments[0].texture = tex;
					pass.colorAttachments[0].loadAction = MTLLoadActionClear;
					pass.colorAttachments[0].storeAction = MTLStoreActionStore;
					pass.colorAttachments[0].clearColor = MTLClearColorMake(32 / 255.0, 32 / 255.0, 32 / 255.0, 1);
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
					check(re != nil, "a render command encoder");
					[re setRenderPipelineState:rps];
					if (variant == 1) {
						[re setVertexBytes:verts length:sizeof(verts) atIndex:0];
					}
					[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
					[re endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
					uint32_t px[64 * 64];
					[tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
					int covered = 0;
					for (int i = 0; i < 64 * 64; i++) {
						covered += px[i] != 0xff202020u;
					}
					uint32_t a = px[1 * 64 + 1], c = px[40 * 64 + 32], rr = px[55 * 64 + 10], gg = px[55 * 64 + 54], bb = px[10 * 64 + 32];
					printf("  pixels: (1,1) %08x (32,40) %08x (10,55) %08x (54,55) %08x (32,10) %08x; %d covered\n", a, c, rr, gg, bb, covered);
					check(a == 0xff202020u, "the corner keeps the clear colour");
					check(c != 0xff202020u, "the centre is drawn");
					check(((rr >> 16) & 255) > ((rr >> 8) & 255) && ((rr >> 16) & 255) > (rr & 255), "bottom-left is red");
					check(((gg >> 8) & 255) > ((gg >> 16) & 255) && ((gg >> 8) & 255) > (gg & 255), "bottom-right is green");
					check((bb & 255) > ((bb >> 16) & 255) && (bb & 255) > ((bb >> 8) & 255), "top is blue");
					check(covered > 500 && covered < 2500, "about a third of the target is covered");
				}
			}
			/* M5.5: a CAMetalLayer drawable (an IOSurface-backed texture), presented */
			if (gFailed == 0) {
				printf("stage 14: a CAMetalLayer drawable, a triangle into it, presentDrawable:\n");
				CAMetalLayer * layer = [CAMetalLayer layer];
				layer.device = device;
				layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
				layer.framebufferOnly = NO;
				layer.drawableSize = CGSizeMake(64, 64);
				id<CAMetalDrawable> drawable = [layer nextDrawable];
				check(drawable != nil, "nextDrawable gave a drawable");
				if (drawable != nil) {
					id<MTLTexture> dt = drawable.texture;
					printf("  drawable texture %lux%lu format %lu, iosurface %p\n", (unsigned long)dt.width, (unsigned long)dt.height,
					    (unsigned long)dt.pixelFormat, (void *)dt.iosurface);
					check(dt.iosurface != NULL, "the drawable texture is over an IOSurface");
					id<MTLFunction> vf = [lib newFunctionWithName:@"tri_vs"], ff = [lib newFunctionWithName:@"tri_fs"];
					MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
					rpd.vertexFunction = vf;
					rpd.fragmentFunction = ff;
					rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
					id<MTLRenderPipelineState> rps = [device newRenderPipelineStateWithDescriptor:rpd error:&err];
					check(rps != nil, "a render pipeline state");
					if (rps != nil && dt.iosurface != NULL) {
						MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
						pass.colorAttachments[0].texture = dt;
						pass.colorAttachments[0].loadAction = MTLLoadActionClear;
						pass.colorAttachments[0].storeAction = MTLStoreActionStore;
						pass.colorAttachments[0].clearColor = MTLClearColorMake(32 / 255.0, 32 / 255.0, 32 / 255.0, 1);
						id<MTLCommandBuffer> cb = [queue commandBuffer];
						id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
						[re setRenderPipelineState:rps];
						[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
						[re endEncoding];
						[cb presentDrawable:drawable];
						[cb commit];
						[cb waitUntilCompleted];
						check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
						IOSurfaceRef surf = dt.iosurface;
						IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
						const uint8_t * base = IOSurfaceGetBaseAddress(surf);
						size_t sp = IOSurfaceGetBytesPerRow(surf);
						#define SPX(x, y) (*(const uint32_t *)(base + (y) * sp + (x) * 4))
						uint32_t a = SPX(1, 1), c = SPX(32, 40), rr = SPX(10, 55), gg = SPX(54, 55), bb = SPX(32, 10);
						int covered = 0;
						for (int y = 0; y < 64; y++) {
							for (int x = 0; x < 64; x++) {
								covered += SPX(x, y) != 0xff202020u;
							}
						}
						IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
						printf("  IOSurface pixels: (1,1) %08x (32,40) %08x (10,55) %08x (54,55) %08x (32,10) %08x; %d covered\n", a, c, rr, gg, bb, covered);
						check(a == 0xff202020u && c != 0xff202020u, "the IOSurface has the clear colour and the triangle");
						check(((rr >> 16) & 255) > ((rr >> 8) & 255) && (bb & 255) > ((bb >> 16) & 255) && ((gg >> 8) & 255) > (gg & 255), "red, green and blue corners");
						check(covered > 500 && covered < 2500, "about a third covered");
					}
				}
			}
			/* M5.6: a fragment function that samples a texture, and blending */
			if (gFailed == 0) {
				printf("stage 15: quad_vs + tex_fs: sample a 4x4 texture (nearest, clamp) into a 64x64 target\n");
				id<MTLFunction> vf = [lib newFunctionWithName:@"quad_vs"], ff = [lib newFunctionWithName:@"tex_fs"];
				check(vf != nil && ff != nil, "quad_vs and tex_fs");
				MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
				rpd.vertexFunction = vf;
				rpd.fragmentFunction = ff;
				rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				id<MTLRenderPipelineState> rps = [device newRenderPipelineStateWithDescriptor:rpd error:&err];
				check(rps != nil, "a render pipeline state with a sampled texture");
				if (rps == nil && err != nil) {
					printf("    %s\n", [[err localizedDescription] UTF8String]);
				}
				MTLTextureDescriptor * sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				    width:4 height:4 mipmapped:NO];
				id<MTLTexture> src = [device newTextureWithDescriptor:sd];
				uint32_t texels[16];
				for (int y = 0; y < 4; y++) {
					for (int x = 0; x < 4; x++) {
						/* RGBA8 in memory R,G,B,A: red, green / blue, yellow quadrants */
						texels[y * 4 + x] = (x < 2 && y < 2) ? 0xff0000ffu : (x >= 2 && y < 2) ? 0xff00ff00u : (x < 2) ? 0xffff0000u : 0xff00ffffu;
					}
				}
				[src replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
				MTLSamplerDescriptor * smd = [MTLSamplerDescriptor new];
				smd.minFilter = MTLSamplerMinMagFilterNearest;
				smd.magFilter = MTLSamplerMinMagFilterNearest;
				smd.sAddressMode = MTLSamplerAddressModeClampToEdge;
				smd.tAddressMode = MTLSamplerAddressModeClampToEdge;
				id<MTLSamplerState> samp = [device newSamplerStateWithDescriptor:smd];
				check(src != nil && samp != nil, "a texture and a sampler state");
				MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				    width:64 height:64 mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				id<MTLTexture> tex = [device newTextureWithDescriptor:td];
				if (gFailed == 0) {
					MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
					pass.colorAttachments[0].texture = tex;
					pass.colorAttachments[0].loadAction = MTLLoadActionClear;
					pass.colorAttachments[0].storeAction = MTLStoreActionStore;
					pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
					[re setRenderPipelineState:rps];
					[re setFragmentTexture:src atIndex:0];
					[re setFragmentSamplerState:samp atIndex:0];
					[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
					[re endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
					uint32_t px[64 * 64];
					[tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
					uint32_t o = px[1 * 64 + 1], tl = px[16 * 64 + 16], tr = px[16 * 64 + 48], bl = px[48 * 64 + 16], br = px[48 * 64 + 48];
					printf("  pixels: outside %08x, top-left %08x, top-right %08x, bottom-left %08x, bottom-right %08x\n", o, tl, tr, bl, br);
					check(o == 0xff000000u, "outside the quad is the clear colour");
					check(tl == 0xffff0000u, "top-left is red (BGRA ffff0000... as stored: red)");
					check(tr == 0xff00ff00u, "top-right is green");
					check(bl == 0xff0000ffu, "bottom-left is blue");
					check(br == 0xffffff00u, "bottom-right is yellow");
				}
			}
			if (gFailed == 0) {
				printf("stage 16: blending: quad_vs + half_fs (red, alpha 0.5), SRC_ALPHA / ONE_MINUS_SRC_ALPHA, over blue\n");
				id<MTLFunction> vf = [lib newFunctionWithName:@"quad_vs"], ff = [lib newFunctionWithName:@"half_fs"];
				MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
				rpd.vertexFunction = vf;
				rpd.fragmentFunction = ff;
				rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				rpd.colorAttachments[0].blendingEnabled = YES;
				rpd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
				rpd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				rpd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
				rpd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
				id<MTLRenderPipelineState> rps = [device newRenderPipelineStateWithDescriptor:rpd error:&err];
				check(rps != nil, "a blending render pipeline state");
				MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				    width:64 height:64 mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget;
				id<MTLTexture> tex = [device newTextureWithDescriptor:td];
				if (gFailed == 0) {
					MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
					pass.colorAttachments[0].texture = tex;
					pass.colorAttachments[0].loadAction = MTLLoadActionClear;
					pass.colorAttachments[0].storeAction = MTLStoreActionStore;
					pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
					[re setRenderPipelineState:rps];
					[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
					[re endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
					uint32_t px[64 * 64];
					[tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
					uint32_t o = px[1 * 64 + 1], c = px[32 * 64 + 32];
					int r = (c >> 16) & 255, b = c & 255, g = (c >> 8) & 255;
					printf("  pixels: outside %08x, centre %08x (r %d g %d b %d)\n", o, c, r, g, b);
					check(o == 0xff0000ffu, "outside the quad is blue");
					check(r > 118 && r < 137 && b > 118 && b < 137 && g < 8, "the centre is half red over half blue");
				}
			}
			/* M5.6: the vertex function reads its vertices from a device buffer (an SSBO) */
			if (gFailed == 0) {
				printf("stage 17: ssbo_vs (vertices from a device buffer), tri_fs\n");
				id<MTLFunction> vf = [lib newFunctionWithName:@"ssbo_vs"], ff = [lib newFunctionWithName:@"tri_fs"];
				check(vf != nil && ff != nil, "ssbo_vs and tri_fs");
				MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
				rpd.vertexFunction = vf;
				rpd.fragmentFunction = ff;
				rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				id<MTLRenderPipelineState> rps = [device newRenderPipelineStateWithDescriptor:rpd error:&err];
				check(rps != nil, "a render pipeline state whose vertex stage has a device buffer");
				static const float verts[6][4] = {
					{ -0.8f, -0.8f, 0, 1 }, { 0.8f, -0.8f, 0, 1 }, { 0.0f, 0.8f, 0, 1 },
					{ 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 },
				};
				id<MTLBuffer> vb = [device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
				MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				    width:64 height:64 mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget;
				id<MTLTexture> tex = [device newTextureWithDescriptor:td];
				if (gFailed == 0) {
					MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
					pass.colorAttachments[0].texture = tex;
					pass.colorAttachments[0].loadAction = MTLLoadActionClear;
					pass.colorAttachments[0].storeAction = MTLStoreActionStore;
					pass.colorAttachments[0].clearColor = MTLClearColorMake(32 / 255.0, 32 / 255.0, 32 / 255.0, 1);
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
					[re setRenderPipelineState:rps];
					[re setVertexBuffer:vb offset:0 atIndex:0];
					[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
					[re endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
					uint32_t px[64 * 64];
					[tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
					int covered = 0;
					for (int i = 0; i < 64 * 64; i++) {
						covered += px[i] != 0xff202020u;
					}
					uint32_t a = px[1 * 64 + 1], c = px[40 * 64 + 32], rr = px[55 * 64 + 10], gg = px[55 * 64 + 54], bb = px[10 * 64 + 32];
					printf("  pixels: (1,1) %08x (32,40) %08x (10,55) %08x (54,55) %08x (32,10) %08x; %d covered\n", a, c, rr, gg, bb, covered);
					check(a == 0xff202020u && c != 0xff202020u, "clear colour at the corner, the triangle in the centre");
					check(((rr >> 16) & 255) > ((rr >> 8) & 255) && (bb & 255) > ((bb >> 16) & 255) && ((gg >> 8) & 255) > (gg & 255), "red, green and blue corners");
					check(covered > 500 && covered < 2500, "about a third covered");
				}
			}
			/* M5.6: three colour attachments written by one fragment function */
			if (gFailed == 0) {
				printf("stage 18: quad_vs + mrt_fs: three render targets\n");
				id<MTLFunction> vf = [lib newFunctionWithName:@"quad_vs"], ff = [lib newFunctionWithName:@"mrt_fs"];
				check(vf != nil && ff != nil, "quad_vs and mrt_fs");
				MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
				rpd.vertexFunction = vf;
				rpd.fragmentFunction = ff;
				for (int i = 0; i < 3; i++) {
					rpd.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
				}
				id<MTLRenderPipelineState> rps = [device newRenderPipelineStateWithDescriptor:rpd error:&err];
				check(rps != nil, "a render pipeline state with three colour attachments");
				if (rps == nil && err != nil) {
					printf("    %s\n", [[err localizedDescription] UTF8String]);
				}
				id<MTLTexture> tx[3];
				for (int i = 0; i < 3; i++) {
					MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
					    width:64 height:64 mipmapped:NO];
					td.usage = MTLTextureUsageRenderTarget;
					tx[i] = [device newTextureWithDescriptor:td];
				}
				if (gFailed == 0) {
					MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
					for (int i = 0; i < 3; i++) {
						pass.colorAttachments[i].texture = tx[i];
						pass.colorAttachments[i].loadAction = MTLLoadActionClear;
						pass.colorAttachments[i].storeAction = MTLStoreActionStore;
						pass.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 1);
					}
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
					[re setRenderPipelineState:rps];
					[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
					[re endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
					uint32_t o[3], c[3], tl[3];
					for (int i = 0; i < 3; i++) {
						uint32_t px[64 * 64];
						[tx[i] getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
						o[i] = px[1 * 64 + 1];
						c[i] = px[32 * 64 + 32];
						tl[i] = px[10 * 64 + 10];
					}
					printf("  target 0: outside %08x centre %08x\n  target 1: outside %08x centre %08x\n  target 2: outside %08x centre %08x top-left %08x\n",
					    o[0], c[0], o[1], c[1], o[2], c[2], tl[2]);
					check(o[0] == 0xff000000u && o[1] == 0xff000000u && o[2] == 0xff000000u, "outside the quad is the clear colour in all three");
					check(c[0] == 0xffff0000u, "target 0 is red");
					check(c[1] == 0xff00ff00u, "target 1 is green");
					check(((c[2] >> 16) & 255) > 100 && ((c[2] >> 16) & 255) < 156 && ((c[2] >> 8) & 255) > 100 && ((c[2] >> 8) & 255) < 156 && (c[2] & 255) == 255,
					    "target 2 is the uv, with blue 1 (centre = 0.5, 0.5)");
					check(((tl[2] >> 16) & 255) < 40 && ((tl[2] >> 8) & 255) < 40, "target 2 is near zero at the top left");
				}
			}
			if (gFailed == 0) {
				printf("stage 19: point_vs + white_fs: a point of size 8\n");
				id<MTLFunction> vf = [lib newFunctionWithName:@"point_vs"], ff = [lib newFunctionWithName:@"white_fs"];
				check(vf != nil && ff != nil, "point_vs and white_fs");
				MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
				rpd.vertexFunction = vf;
				rpd.fragmentFunction = ff;
				rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				id<MTLRenderPipelineState> rps = [device newRenderPipelineStateWithDescriptor:rpd error:&err];
				check(rps != nil, "a render pipeline state");
				MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				    width:64 height:64 mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget;
				id<MTLTexture> tex = [device newTextureWithDescriptor:td];
				if (gFailed == 0) {
					MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
					pass.colorAttachments[0].texture = tex;
					pass.colorAttachments[0].loadAction = MTLLoadActionClear;
					pass.colorAttachments[0].storeAction = MTLStoreActionStore;
					pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
					id<MTLCommandBuffer> cb = [queue commandBuffer];
					id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
					[re setRenderPipelineState:rps];
					[re drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
					[re endEncoding];
					[cb commit];
					[cb waitUntilCompleted];
					check([cb status] == MTLCommandBufferStatusCompleted, "the command buffer completed");
					uint32_t px[64 * 64];
					[tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
					int white = 0, minx = 64, maxx = -1;
					for (int i = 0; i < 64 * 64; i++) {
						if (px[i] == 0xffffffffu) {
							white++;
							minx = (i % 64) < minx ? (i % 64) : minx;
							maxx = (i % 64) > maxx ? (i % 64) : maxx;
						}
					}
					printf("  %d white pixels, x %d..%d\n", white, minx, maxx);
					check(white >= 36 && white <= 100, "the point is about 8x8 pixels");
					check(px[32 * 64 + 32] == 0xffffffffu, "the centre is white");
				}
			}
		}

		/* the service holds 64 buffers: the plug-in carves the rest out of slabs */
		if (gFailed == 0) {
			printf("stage 21: 300 buffers alive at once, each its own bytes\n");
			const int N = 300;
			NSMutableArray * bufs = [NSMutableArray array];
			size_t sizes[300];
			int madeAll = 1, intact = 1, zeroed = 1;

			for (int i = 0; i < N; i++) {
				sizes[i] = 4096 + (size_t)(i % 7) * 4096;
				id<MTLBuffer> b = [device newBufferWithLength:sizes[i] options:MTLResourceStorageModeShared];

				if (b == nil) {
					madeAll = 0;
					break;
				}
				uint8_t * p = [b contents];
				for (size_t k = 0; k < sizes[i]; k++) {
					zeroed &= p[k] == 0;
				}
				memset(p, (int)(i & 0xff) | 1, sizes[i]);
				[bufs addObject:b];
			}
			check(madeAll, "300 buffers were made");
			check(zeroed, "every one read zero when new");
			for (int i = 0; i < (int)[bufs count]; i++) {
				id<MTLBuffer> bi = bufs[i];
				uint8_t * p = [bi contents];
				for (size_t k = 0; k < sizes[i]; k++) {
					intact &= p[k] == (uint8_t)((i & 0xff) | 1);
				}
			}
			check(intact, "none overwrote another");
			[bufs removeObjectsInRange:NSMakeRange(0, [bufs count] / 2)];
			id<MTLBuffer> again = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
			check(again != nil && ((uint8_t *)[again contents])[100] == 0, "after freeing half, a new buffer is made and is zero");
			id<MTLBuffer> big = [device newBufferWithLength:6u << 20 options:MTLResourceStorageModeShared];
			printf("  a 6 MB buffer of its own: %s (physically contiguous memory below 4 GB is nearly all gone after boot)\n", big != nil ? "made" : "not available");
		}

		kr = metalProps(conn, 0);
		printf("properties off: kr %#x\n", kr);
		IOServiceClose(conn);
		printf("%s\n", gFailed == 0 ? "PASSED" : "FAILED");
		return gFailed == 0 ? 0 : 1;
	}
}
