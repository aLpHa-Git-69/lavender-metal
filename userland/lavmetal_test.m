/*
 * lavmetal_test: M5.1's check. Asks the LavenderGPU service to name the Metal plug-in on IOAcceleratorES, then asks Metal for
 * its device the way every program does, and says what it got. The plug-in's own log is /var/root/lavmetal.log.
 *
 * Build (on the Mac): see userland/build-lavmetal.sh
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>

enum { kLavMetal = 5 };

static kern_return_t
metalProps(io_connect_t conn, uint32_t on)
{
	return IOConnectCallStructMethod(conn, kLavMetal, &on, sizeof(on), NULL, NULL);
}

int
main(void)
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

		/* what Metal would find before the properties are there */
		io_service_t accel = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
		printf("IOAcceleratorES service: %s\n", accel != 0 ? "present" : "absent");
		if (accel != 0) {
			CFTypeRef n = IORegistryEntryCreateCFProperty(accel, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
			printf("  MetalPluginName before: %s\n", n != NULL ? "set" : "none");
			if (n != NULL) {
				CFRelease(n);
			}
		}

		kr = metalProps(conn, 1);
		printf("properties on: kr %#x\n", kr);
		if (accel != 0) {
			CFTypeRef n = IORegistryEntryCreateCFProperty(accel, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
			CFTypeRef c = IORegistryEntryCreateCFProperty(accel, CFSTR("MetalPluginClassName"), kCFAllocatorDefault, 0);
			NSLog(@"MetalPluginName %@, MetalPluginClassName %@", (__bridge id)n, (__bridge id)c);
			if (n != NULL) {
				CFRelease(n);
			}
			if (c != NULL) {
				CFRelease(c);
			}
		}

		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		printf("MTLCreateSystemDefaultDevice: %s\n", device != nil ? "a device" : "nil");
		if (device != nil) {
			printf("  class %s\n", object_getClassName(device));
			printf("  name  %s\n", [[device name] UTF8String]);
			printf("  registryID %llu\n", (unsigned long long)[device registryID]);
			id q = [device newCommandQueue];
			printf("  newCommandQueue: %s (not expected to work yet)\n", q != nil ? "an object" : "nil");
		}

		kr = metalProps(conn, 0);
		printf("properties off: kr %#x\n", kr);
		IOServiceClose(conn);
		printf("%s\n", device != nil ? "PASSED" : "FAILED");
		return device != nil ? 0 : 1;
	}
}
