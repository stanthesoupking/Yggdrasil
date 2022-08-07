#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "yggdrasil/yggdrasil.h"

int _multiply_by_two(Ygg_Context* context, int in) {
	return in * 2;
}
ygg_fiber_declare_inout(multiply_by_two, int, int, _multiply_by_two);

int main(int argc, const char * argv[]) {
	Ygg_Coordinator_Parameters parameters = {
		.thread_count = 8,
	};
	
	Ygg_Coordinator* coordinator = ygg_coordinator_new(parameters);
	Ygg_Context* context = ygg_blocking_context_new(coordinator);
	
	for (int i = 0; i < 256; ++i) {
		int result = multiply_by_two_dispatch_sync(context, Ygg_Priority_Normal, i);
		printf("Result = %d\n", result);
	}
	
	printf("Done.\n");
	
	ygg_blocking_context_destroy(context);
	ygg_coordinator_destroy(coordinator);
	
	return 0;
}
