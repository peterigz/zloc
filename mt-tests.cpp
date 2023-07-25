#define TLOC_ERROR_COLOR "\033[90m"
#define TLOC_IMPLEMENTATION
#define TLOC_OUTPUT_ERROR_MESSAGES
#include <stdio.h>

#include "2loc.h"

#define tloc_free_memory(memory) if(memory) free(memory);

void PrintTestResult(const char *message, int result) {
	printf("%s", message);
	printf("%s [%s]\033[0m\n", result == 0 ? "\033[31m" : "\033[32m", result == 0 ? "FAILED" : "PASSED");
}