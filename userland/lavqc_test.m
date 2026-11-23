/*
 * lavqc_test: QuartzCore's own pipelines drawn on the Adreno through the Metal plug-in (M5.6).
 *
 *   lavqc_test lib.metallib [max_per_vertex]    for each vertex function of the library, up to MAX_PER_VERTEX fragment functions
 *                                               whose pipeline state the plug-in makes (default 2); each is drawn once into three
 *                                               64x64 targets with every buffer slot bound to zeroed memory and every texture slot to a
 *                                               4x4 texture, and the command buffer's status is checked.
 *   lavqc_test lib.metallib vs fs               just that pair
 *
 * Nothing here knows what CoreAnimation puts in the buffers: the point is that the shaders the compositor uses run to completion on the
 * GPU (no hang, no fault). Build (on the Mac): userland/build-lavmetal.sh
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

static unsigned gDrawn, gDrawFailed, gNoPipe;

static id<MTLDevice> gDevice;
static id<MTLCommandQueue> gQueue;
static id<MTLBuffer> gZero;
static id<MTLTexture> gSrc, gTargets[3];
static id<MTLSamplerState> gSamp;

static void
drawPair1(id<MTLLibrary> lib, NSString * vn, NSString * fn, BOOL * made)
{
	NSError * err = nil;
	id<MTLFunction> vf = [lib newFunctionWithName:vn], ff = [lib newFunctionWithName:fn];
	MTLRenderPipelineDescriptor * rpd = [MTLRenderPipelineDescriptor new];
	id<MTLRenderPipelineState> rps;

	*made = NO;
	if (vf == nil || ff == nil) {
		return;
	}
	rpd.vertexFunction = vf;
	rpd.fragmentFunction = ff;
	for (int i = 0; i < 3; i++) {
		rpd.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
	}
	/* the vertex descriptor CoreAnimation would give: this test does not know the attribute count, so try 0..6 float4 attributes
	 * until the plug-in's count matches (a link or fragment refusal stops the search) */
	rps = nil;
	for (int k = 0; k < 7 && rps == nil; k++) {
		MTLVertexDescriptor * vd = [MTLVertexDescriptor vertexDescriptor];

		for (int i = 0; i < k; i++) {
			vd.attributes[i].format = MTLVertexFormatFloat4;
			vd.attributes[i].offset = 0;
			vd.attributes[i].bufferIndex = 30;
		}
		vd.layouts[30].stride = 16;
		rpd.vertexDescriptor = vd;
		err = nil;
		rps = [gDevice newRenderPipelineStateWithDescriptor:rpd error:&err];
		if (rps == nil && err != nil && ([[err localizedDescription] containsString:@"link:"] ||
		    [[err localizedDescription] containsString:@"fixed_frag"] || [[err localizedDescription] containsString:@"fragment function"])) {
			break;
		}
	}
	if (rps == nil) {
		gNoPipe++;
		printf("NOPIPE %s + %s: %s\n", [vn UTF8String], [fn UTF8String], err ? [[err localizedDescription] UTF8String] : "?");
		return;
	}
	*made = YES;
	printf("DRAW   %s + %s\n", [vn UTF8String], [fn UTF8String]);
	MTLRenderPassDescriptor * pass = [MTLRenderPassDescriptor renderPassDescriptor];
	for (int i = 0; i < 3; i++) {
		pass.colorAttachments[i].texture = gTargets[i];
		pass.colorAttachments[i].loadAction = MTLLoadActionClear;
		pass.colorAttachments[i].storeAction = MTLStoreActionStore;
		pass.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 1);
	}
	id<MTLCommandBuffer> cb = [gQueue commandBuffer];
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
	[re setRenderPipelineState:rps];
	for (NSUInteger i = 0; i < 31; i++) {
		[re setVertexBuffer:gZero offset:0 atIndex:i];
		[re setFragmentBuffer:gZero offset:0 atIndex:i];
	}
	for (NSUInteger i = 0; i < 16; i++) {
		[re setFragmentTexture:gSrc atIndex:i];
		[re setFragmentSamplerState:gSamp atIndex:i];
	}
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if ([cb status] == MTLCommandBufferStatusCompleted) {
		gDrawn++;
	} else {
		gDrawFailed++;
		printf("  FAILED: command buffer status %ld\n", (long)[cb status]);
	}
}

/* one pair per autorelease pool: the command buffer holds the pipeline (two of the service's 64 buffers) until the pool drains */
static void
drawPair(id<MTLLibrary> lib, NSString * vn, NSString * fn, BOOL * made)
{
	@autoreleasepool {
		drawPair1(lib, vn, fn, made);
	}
}

int
main(int argc, char ** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	@autoreleasepool {
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
		io_connect_t conn = 0;
		NSError * err = nil;
		int maxPer = 2;

		if (argc < 2) {
			printf("usage: lavqc_test lib.metallib [max_per_vertex | vs fs]\n");
			return 1;
		}
		if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
			printf("no LavenderGPU service\n");
			return 1;
		}
		gConn = conn;
		signal(SIGABRT, crashed);
		signal(SIGSEGV, crashed);
		signal(SIGTRAP, crashed);
		signal(SIGILL, crashed);
		metalProps(conn, 1);
		gDevice = MTLCreateSystemDefaultDevice();
		if (gDevice == nil) {
			printf("MTLCreateSystemDefaultDevice: nil\nFAILED\n");
			metalProps(conn, 0);
			return 1;
		}
		gQueue = [gDevice newCommandQueue];
		gZero = [gDevice newBufferWithLength:65536 options:MTLResourceStorageModeShared];
		MTLTextureDescriptor * sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		    width:4 height:4 mipmapped:NO];
		gSrc = [gDevice newTextureWithDescriptor:sd];
		uint32_t texels[16];
		for (int i = 0; i < 16; i++) {
			texels[i] = 0xff808080u;
		}
		[gSrc replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
		MTLSamplerDescriptor * smd = [MTLSamplerDescriptor new];
		smd.minFilter = MTLSamplerMinMagFilterLinear;
		smd.magFilter = MTLSamplerMinMagFilterLinear;
		gSamp = [gDevice newSamplerStateWithDescriptor:smd];
		for (int i = 0; i < 3; i++) {
			MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			    width:64 height:64 mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			gTargets[i] = [gDevice newTextureWithDescriptor:td];
		}
		if (gQueue == nil || gZero == nil || gSrc == nil || gSamp == nil || gTargets[2] == nil) {
			printf("setup failed\nFAILED\n");
			metalProps(conn, 0);
			return 1;
		}

		NSData * data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
		dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		id<MTLLibrary> lib = data ? [gDevice newLibraryWithData:dd error:&err] : nil;
		if (lib == nil) {
			printf("no library from %s\nFAILED\n", argv[1]);
			metalProps(conn, 0);
			return 1;
		}
		if (argc == 4) {
			BOOL made;

			drawPair(lib, [NSString stringWithUTF8String:argv[2]], [NSString stringWithUTF8String:argv[3]], &made);
		} else {
			NSMutableArray * vs = [NSMutableArray array], * fs = [NSMutableArray array];

			if (argc == 3) {
				maxPer = atoi(argv[2]);
			}
			for (NSString * n in [lib functionNames]) {
				id<MTLFunction> f = [lib newFunctionWithName:n];

				if ([f functionType] == MTLFunctionTypeVertex) {
					[vs addObject:n];
				} else if ([f functionType] == MTLFunctionTypeFragment) {
					[fs addObject:n];
				}
			}
			printf("%lu vertex functions, %lu fragment functions\n", (unsigned long)[vs count], (unsigned long)[fs count]);
			for (NSString * v in vs) {
				int done = 0, tries = 0;

				for (NSString * f in fs) {
					BOOL made = NO;

					if (done >= maxPer || tries >= 60) {
						break;
					}
					tries++;
					drawPair(lib, v, f, &made);
					done += made;
				}
			}
		}
		printf("drawn %u, failed %u, no pipeline %u\n", gDrawn, gDrawFailed, gNoPipe);
		metalProps(conn, 0);
		printf("%s\n", gDrawFailed == 0 && gDrawn > 0 ? "PASSED" : "FAILED");
		return gDrawFailed == 0 && gDrawn > 0 ? 0 : 1;
	}
}
