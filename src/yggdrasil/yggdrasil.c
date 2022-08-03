
#include "yggdrasil.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#define ygg_assert(cond, ...) \
if(!(cond)) { \
	printf("------------------------------------------------------------\n"); \
	printf("  Assertion failed on line %d of %s:  \n\t", __LINE__, __FILENAME__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
	printf("------------------------------------------------------------\n"); \
	abort();\
}

#define ygg_abort(...) \
printf("------------------------------------------------------------\n"); \
printf("  Abort on line %d of %s:  \n\t", __LINE__, __FILENAME__); \
printf(__VA_ARGS__); \
printf("\n"); \
printf("------------------------------------------------------------\n"); \
abort();

#include "ygg_cpu.c"
#include "ygg_semaphore.c"
#include "ygg_spinlock.c"
#include "ygg_coordinator.c"

