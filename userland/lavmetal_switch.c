/* lavmetal_switch on|off: put the Metal plug-in properties on IOAcceleratorES (or take them away), so that processes started
 * afterwards (backboardd after a restart) get the LavenderMetal device. The properties stay until `off`. */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char ** argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("LavenderGPU"));
	io_connect_t conn = 0;
	uint32_t on = argc > 1 && strcmp(argv[1], "on") == 0;
	kern_return_t kr;

	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		printf("no LavenderGPU service\n");
		return 1;
	}
	kr = IOConnectCallStructMethod(conn, 5, &on, sizeof(on), NULL, NULL);
	printf("plug-in %s: kr %#x\n", on ? "on" : "off", kr);
	return kr != 0;
}
