#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "yggdrasil/yggdrasil.h"

void _fiber_encode_buffer(Ygg_Context* ctx, void* args) {
	printf("_fiber_encode_buffer: start\n");
	
	sleep(1);
	
	printf("_fiber_encode_buffer: end\n");
	
	int result = 1234;
	ygg_store_result(ctx, &result, sizeof(result));
}

void _fiber_visibility_check(Ygg_Context* ctx, void* args) {
	printf("_fiber_visibility_check: start\n");
	
	// Encode command buffer
	Ygg_Future* encode_future = ygg_coordinator_dispatch(ygg_fiber_coordinator(ctx), ygg_fiber("encode_visibility", _fiber_encode_buffer), Ygg_Priority_Normal, NULL, 0);
	const int* result = ygg_future_unwrap(encode_future, ctx);
	printf("encode returned %d\n", *result);
	ygg_future_release(encode_future);
	
	printf("_fiber_visibility_check: end\n");
}

int main(int argc, const char * argv[]) {
	Ygg_Coordinator_Parameters parameters = {
		.thread_count = 8,
	};
	
	Ygg_Coordinator* coordinator = ygg_coordinator_new(parameters);
	
	Ygg_Context* context = ygg_blocking_context_new(coordinator);
			
	for (int i = 0; i < 256; ++i) {
		Ygg_Future* visibility_checking_result = ygg_coordinator_dispatch(coordinator, ygg_fiber("visibility_check", _fiber_visibility_check), Ygg_Priority_Normal, NULL, 0);
		ygg_future_wait(visibility_checking_result, context);
		ygg_future_release(visibility_checking_result);
		printf("Kicking of next visibility test...\n");
	}
	
	printf("Done.\n");
	
	ygg_blocking_context_destroy(context);
	ygg_coordinator_destroy(coordinator);
	
	return 0;
}
