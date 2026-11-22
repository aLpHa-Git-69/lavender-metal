/*
 * LavenderMetal: the plug-in Metal loads when the kernel service names it (M5.1).
 *
 * How Metal finds a device (the system's own code, MTLRegisterDevices): it matches the IOKit class IOAcceleratorES, reads the
 * service's MetalPluginName ("LavenderMetal") and MetalPluginClassName ("LavenderMetalDevice"), opens
 * /System/Library/Extensions/LavenderMetal.bundle, takes that class, insists that it is a subclass of Metal's private
 * _MTLDevice, and sends it -initWithAcceleratorPort:. That object is the process's device.
 *
 * This first step is a device that exists and nothing more: it answers the initialiser and a few identity questions, and for
 * every other message Metal sends it, it writes "asked and not implemented: -selector" to /var/root/lavmetal.log and answers
 * 0 / nil. That list, in order, is what the next steps implement.
 *
 * _MTLDevice is not in the SDK (it lives in the system's copy of Metal.framework), so the class is made at run time, in the
 * constructor that runs when the bundle is loaded, by objc_allocateClassPair on the _MTLDevice that is there.
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LAV_LOG "/var/root/lavmetal.log"

static Class gBase;                     /* Metal's private _MTLDevice */
static unsigned int gPort;              /* the IOAcceleratorES service this device was made for */

static void
lavlog(const char * fmt, ...)
{
	char line[512];
	va_list ap;
	FILE * f;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	f = fopen(LAV_LOG, "a");
	if (f != NULL) {
		fprintf(f, "%ld LavenderMetal[%d]: %s\n", (long)time(NULL), (int)getpid(), line);
		fclose(f);
	}
	NSLog(@"LavenderMetal: %s", line);
}

/* -initWithAcceleratorPort: the last step of how Metal finds a device */
static id
deviceInit(id self, SEL _cmd, unsigned int port)
{
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL) = (id (*)(struct objc_super *, SEL))objc_msgSendSuper;

	self = send(&sup, @selector(init));
	gPort = port;
	lavlog("a device for the accelerator port %u (%s)", port, self != nil ? "made" : "_MTLDevice's init gave nil");
	return self;
}

static id
deviceName(__unused id self, __unused SEL _cmd)
{
	return @"Lavender Adreno 512 (Metal: a device that exists)";
}

static uint64_t
deviceRegistryID(__unused id self, __unused SEL _cmd)
{
	return gPort;
}

/* the answer to every message nothing implements: written down, and 0 */
static uintptr_t
notImplemented(id self, SEL _cmd)
{
	lavlog("asked and not implemented: -%s (%s)", sel_getName(_cmd), class_getName(object_getClass(self)));
	return 0;
}

static BOOL
resolveInstanceMethod(Class cls, __unused SEL _cmd, SEL sel)
{
	/* only what is not there yet; the generic IMP answers with 0, which is nil, NO and 0 alike */
	return class_addMethod(cls, sel, (IMP)notImplemented, "Q@:");
}

__attribute__((constructor)) static void
lavenderMetalLoad(void)
{
	Class cls;

	gBase = objc_getClass("_MTLDevice");
	if (gBase == Nil) {
		lavlog("the system has no _MTLDevice; this plug-in registers nothing");
		return;
	}
	cls = objc_allocateClassPair(gBase, "LavenderMetalDevice", 0);
	if (cls == Nil) {
		lavlog("LavenderMetalDevice could not be made (already there?)");
		return;
	}
	class_addMethod(cls, @selector(initWithAcceleratorPort:), (IMP)deviceInit, "@@:I");
	class_addMethod(cls, @selector(name), (IMP)deviceName, "@@:");
	class_addMethod(cls, @selector(registryID), (IMP)deviceRegistryID, "Q@:");
	class_addMethod(object_getClass(cls), @selector(resolveInstanceMethod:), (IMP)resolveInstanceMethod, "B@::");
	objc_registerClassPair(cls);
	lavlog("loaded: the class LavenderMetalDevice is registered (base %s)", class_getName(gBase));
}
