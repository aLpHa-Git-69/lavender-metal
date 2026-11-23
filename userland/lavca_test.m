/*
 * lavca_test: CoreAnimation's own renderer drawing a layer tree on the Adreno through the Metal plug-in (M5.6).
 *
 * CARenderer (private on iOS: declared below) renders a CALayer tree into a Metal texture using QuartzCore's compiled pipelines and the
 * real buffers it builds, which is what backboardd does for the screen. This test builds a red layer with a green and a blue child, renders
 * it into a 256x256 BGRA texture on the GPU and reads the pixels back:
 *
 *   lavca_test            run, check three probe pixels (red background, green child, blue child)
 *
 * Build (on the Mac): userland/build-lavmetal.sh
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>

@interface CARenderer : NSObject
+ (CARenderer *)rendererWithMTLTexture:(id<MTLTexture>)tex options:(NSDictionary *)opts;
@property (retain) CALayer * layer;
@property CGRect bounds;
- (void)beginFrameAtTime:(CFTimeInterval)t timeStamp:(void *)ts;
- (void)addUpdateRect:(CGRect)r;
- (void)render;
- (CFTimeInterval)nextFrameTime;
- (void)endFrame;
@end

enum { kLavMetal = 5 };
static io_connect_t gConn;

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
check(const char * what, BOOL ok)
{
	printf("  %-60s %s\n", what, ok ? "OK" : "FAILED");
	if (!ok) {
		gFailed++;
	}
}

static BOOL
near(const uint8_t * px, int b, int g, int r)
{
	return abs(px[0] - b) <= 3 && abs(px[1] - g) <= 3 && abs(px[2] - r) <= 3;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	@autoreleasepool {
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
		io_connect_t conn = 0;
		uint32_t on = 1;

		if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
			printf("no LavenderGPU service\n");
			return 1;
		}
		gConn = conn;
		signal(SIGABRT, crashed);
		signal(SIGSEGV, crashed);
		signal(SIGTRAP, crashed);
		IOConnectCallStructMethod(conn, kLavMetal, &on, sizeof(on), NULL, NULL);

		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (dev == nil) {
			printf("MTLCreateSystemDefaultDevice: nil\nFAILED\n");
			return 1;
		}
		MTLTextureDescriptor * td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:256 height:256 mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
		check("render target texture 256x256", tex != nil);

		CARenderer * r = [CARenderer rendererWithMTLTexture:tex options:nil];
		check("CARenderer made", r != nil);
		if (r == nil || tex == nil) {
			printf("FAILED\n");
			return 1;
		}
		CALayer * root = [CALayer layer];
		root.frame = CGRectMake(0, 0, 256, 256);
		root.backgroundColor = CGColorCreateGenericRGB(1, 0, 0, 1);
		CALayer * g = [CALayer layer];
		g.frame = CGRectMake(32, 32, 64, 64);
		g.backgroundColor = CGColorCreateGenericRGB(0, 1, 0, 1);
		[root addSublayer:g];
		CALayer * b = [CALayer layer];
		b.frame = CGRectMake(160, 160, 64, 64);
		b.backgroundColor = CGColorCreateGenericRGB(0, 0, 1, 1);
		[root addSublayer:b];
		r.layer = root;
		r.bounds = CGRectMake(0, 0, 256, 256);

		[CATransaction flush];
		[r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
		[r addUpdateRect:r.bounds];
		[r render];
		[r endFrame];

		uint8_t px[256 * 256 * 4];
		[tex getBytes:px bytesPerRow:256 * 4 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
		/* CALayer's y axis may be flipped by the renderer: accept either orientation for the two children */
		const uint8_t * bg = px + (8 * 256 + 8) * 4;
		const uint8_t * c1 = px + (64 * 256 + 64) * 4, * c1f = px + (192 * 256 + 64) * 4;
		const uint8_t * c2 = px + (192 * 256 + 192) * 4, * c2f = px + (64 * 256 + 192) * 4;

		printf("pixels: bg %02x%02x%02x  g %02x%02x%02x/%02x%02x%02x  b %02x%02x%02x/%02x%02x%02x\n", bg[2], bg[1], bg[0],
		    c1[2], c1[1], c1[0], c1f[2], c1f[1], c1f[0], c2[2], c2[1], c2[0], c2f[2], c2f[1], c2f[0]);
		check("background is red", near(bg, 0, 0, 255));
		check("green child is where it was put", near(c1, 0, 255, 0) || near(c1f, 0, 255, 0));
		check("blue child is where it was put", near(c2, 255, 0, 0) || near(c2f, 255, 0, 0));

		on = 0;
		IOConnectCallStructMethod(conn, kLavMetal, &on, sizeof(on), NULL, NULL);
		printf("%s\n", gFailed == 0 ? "PASSED" : "FAILED");
		return gFailed == 0 ? 0 : 1;
	}
}
