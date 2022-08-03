#include <stdio.h>
#include <unistd.h>

#include "yggdrasil/yggdrasil.h"

void _fiber_encode_buffer(Ygg_Fiber_Ctx* ctx) {
	printf("_fiber_encode_buffer: start\n");
	
	sleep(1);
	
	printf("_fiber_encode_buffer: end\n");
}

void _fiber_visibility_check(Ygg_Fiber_Ctx* ctx) {
	printf("_fiber_visibility_check: start\n");
	
	// Encode command buffer
	Ygg_Lazy_Result* encode_result = ygg_coordinator_dispatch(ygg_fiber_coordinator(ctx), ygg_fiber("encode_visibility", _fiber_encode_buffer));
	ygg_lazy_result_unwrap(ctx, encode_result);
	ygg_lazy_result_release(encode_result);
	
	printf("_fiber_visibility_check: end\n");
}

int main(int argc, const char * argv[]) {
	// TODO: Add support for multiple threads
	Ygg_Coordinator_Parameters parameters = {
		.thread_count = 1,
	};
	
	Ygg_Coordinator* coordinator = ygg_coordinator_new(parameters);
			
	Ygg_Lazy_Result* visibility_checking_result = ygg_coordinator_dispatch(coordinator, ygg_fiber("visibility_check", _fiber_visibility_check));
	ygg_lazy_result_release(visibility_checking_result);
	
	sleep(10000);
	
	ygg_coordinator_destroy(coordinator);
		
	// Lazy results (or a 'promise') (await on result to unwrap?)
	// terminology: suspend and resume?
	
	return 0;
}
