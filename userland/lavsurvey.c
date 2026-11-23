/* lavsurvey: for every function of a .metallib, whether the compiler of the plug-in serves it, and why not. CPU only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lavcc.h"

int
main(int argc, char ** argv)
{
	FILE * f;
	long size;
	uint8_t * data;
	char why[512], sum[256];
	struct lavcc_lib * lib;
	unsigned ok = 0, bad = 0;
	int quiet = argc > 2;
	int pairs = argc > 2 && strcmp(argv[2], "pairs") == 0;

	if (argc < 2 || (f = fopen(argv[1], "rb")) == NULL) {
		printf("usage: lavsurvey lib.metallib [quiet]\n");
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
		printf("%s: %s\n", argv[1], why);
		return 1;
	}
	if (pairs) {
		lavcc_survey_pairs(lib);
		return 0;
	}
	for (unsigned i = 0; i < lavcc_function_count(lib); i++) {
		int type = 0, r;
		const char * name = lavcc_function_name(lib, i, &type);

		why[0] = 0;
		r = lavcc_probe(lib, i, sum, sizeof(sum), why, sizeof(why));
		if (r) {
			ok++;
			if (!quiet) {
				printf("OK   %-48s %s\n", name, sum);
			}
		} else {
			bad++;
			printf("FAIL %-48s [%s] %s\n", name, type == 0 ? "vertex" : type == 1 ? "fragment" : "kernel", why);
		}
	}
	printf("%s: %u functions served, %u not\n", argv[1], ok, bad);
	return 0;
}
