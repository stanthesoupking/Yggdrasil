
#define YGG_FIBER_STACK_SIZE 128 * 1024
#define YGG_MAXIMUM_FIBERS 1024
#define YGG_QUEUE_SIZE 1024

typedef struct Ygg_Fiber_Ctx {
	Ygg_Coordinator* coordinator;
	Ygg_Fiber_Handle fiber_handle;
} Ygg_Fiber_Ctx;

typedef struct Ygg_Lazy_Result {
	Ygg_Coordinator* coordinator;
	Ygg_Fiber_Handle fiber_handle;
	atomic_uint rc;
	
	// some other data here for result
} Ygg_Lazy_Result;

typedef enum Ygg_Fiber_Internal_State {
	Ygg_Fiber_Internal_State_Not_Started,
	Ygg_Fiber_Internal_State_Running,
	Ygg_Fiber_Internal_State_Suspended,
	Ygg_Fiber_Internal_State_Complete,
} Ygg_Fiber_Internal_State;

typedef struct Ygg_Fiber_Internal {
	unsigned int generation;
	Ygg_Fiber_Internal_State state;
	Ygg_Fiber fiber;
	
	// Valid after the fiber has been started
	Ygg_Worker_Thread* owner_thread;
	
	Ygg_Lazy_Result lazy_result;
	Ygg_Spinlock spinlock;
	
	atomic_uint rc;
	atomic_uint counter;
	
	Ygg_CPU_State resume_state;
	Ygg_CPU_State suspend_state;
	
	// TODO: Allocate elsewhere to decrease struct size
	void* stack;
		
	// TODO: Linked list with pool to support arbitrary array length
	Ygg_Fiber_Handle successors[16];
	unsigned int successor_count;
} Ygg_Fiber_Internal;

typedef struct Ygg_Coordinator {
	Ygg_Fiber_Internal fiber_storage[YGG_MAXIMUM_FIBERS];
	unsigned int fiber_count;
	
	unsigned int fiber_freelist[YGG_MAXIMUM_FIBERS];
	unsigned int fiber_freelist_length;
	Ygg_Spinlock fiber_freelist_spinlock;
		
	Ygg_Semaphore semaphore;
	Ygg_Fiber_Queue fiber_queues[YGG_PRIORITY_COUNT];
	
	Ygg_Worker_Thread** worker_threads;
	unsigned int worker_thread_count;
} Ygg_Coordinator;

Ygg_Coordinator* ygg_coordinator_new(Ygg_Coordinator_Parameters parameters) {
	Ygg_Coordinator* coordinator = calloc(sizeof(Ygg_Coordinator), 1);
		
	*coordinator = (Ygg_Coordinator) {
		.fiber_count = 0,
		.fiber_freelist_length = YGG_MAXIMUM_FIBERS,
				
		.worker_thread_count = parameters.thread_count,
	};
	
	for (unsigned int fiber_index = 0; fiber_index < YGG_MAXIMUM_FIBERS; ++fiber_index) {
		coordinator->fiber_freelist[YGG_MAXIMUM_FIBERS - fiber_index - 1] = fiber_index;
	}
	
	ygg_semaphore_init(&coordinator->semaphore);
	
	for (unsigned int queue_index = 0; queue_index < YGG_PRIORITY_COUNT; ++queue_index) {
		ygg_fiber_queue_init(coordinator->fiber_queues + queue_index, YGG_QUEUE_SIZE);
	}
	
	coordinator->worker_threads = malloc(sizeof(Ygg_Worker_Thread*) * parameters.thread_count);
	for (unsigned int thread_index = 0; thread_index < parameters.thread_count; ++thread_index) {
		coordinator->worker_threads[thread_index] = ygg_worker_thread_new(coordinator, thread_index);
		ygg_worker_thread_start(coordinator->worker_threads[thread_index]);
	}
	
	return coordinator;
}
void ygg_coordinator_destroy(Ygg_Coordinator* coordinator) {
	for (unsigned int queue_index = 0; queue_index < YGG_PRIORITY_COUNT; ++queue_index) {
		ygg_fiber_queue_deinit(coordinator->fiber_queues + queue_index);
	}
	*coordinator = (Ygg_Coordinator) { };
	free(coordinator);
}

ygg_internal void ygg_coordinator_push_fiber(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle, Ygg_Priority priority) {
	ygg_fiber_queue_push(coordinator->fiber_queues + priority, handle);
	ygg_semaphore_signal(&coordinator->semaphore);
}
ygg_internal bool ygg_coordinator_pop_fiber(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle* handle) {
	for (int queue_index = YGG_PRIORITY_COUNT - 1; queue_index > 0; --queue_index) {
		if (ygg_fiber_queue_pop(coordinator->fiber_queues + queue_index, handle)) {
			return true;
		}
	}
	return false;
}

Ygg_Fiber_Internal* ygg_coordinator_deref_fiber_handle(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	ygg_assert(handle.index < YGG_MAXIMUM_FIBERS, "Fiber index out of bounds");
	
	Ygg_Fiber_Internal* internal = coordinator->fiber_storage + handle.index;
	ygg_assert(internal->generation == handle.generation, "Handle generation is out of date");
	
	return internal;
}
void ygg_coordinator_fiber_release(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(coordinator, handle);
	unsigned int previous = atomic_fetch_sub_explicit(&internal->rc, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Fiber over released");
	
	if (previous == 1) {
		// Free fiber from system
		
		// TODO: Pool these or something, don't use free/malloc
		free(internal->stack);
		
		unsigned int new_generation = internal->generation + 1;
		*internal = (Ygg_Fiber_Internal) {
			.generation = new_generation,
		};
		
		ygg_spinlock_lock(&coordinator->fiber_freelist_spinlock);
		coordinator->fiber_freelist[coordinator->fiber_freelist_length++] = handle.index;
		coordinator->fiber_count--;
		ygg_spinlock_unlock(&coordinator->fiber_freelist_spinlock);
	}
}

Ygg_Lazy_Result* ygg_coordinator_dispatch(Ygg_Coordinator* coordinator, Ygg_Fiber fiber, Ygg_Priority priority) {
	unsigned int fiber_index;
	ygg_spinlock_lock(&coordinator->fiber_freelist_spinlock);
	ygg_assert(coordinator->fiber_freelist_length > 0, "Maximum fibers exceeded");
	fiber_index = coordinator->fiber_freelist[--coordinator->fiber_freelist_length];
	coordinator->fiber_count++;
	ygg_spinlock_unlock(&coordinator->fiber_freelist_spinlock);
		
	Ygg_Fiber_Internal* internal = coordinator->fiber_storage + fiber_index;
	unsigned int fiber_generation = internal->generation + 1;
	Ygg_Fiber_Handle handle = (Ygg_Fiber_Handle) {
		.index = fiber_index,
		.generation = fiber_generation,
	};
	
	Ygg_Lazy_Result lazy_result = {
		.coordinator = coordinator,
		.fiber_handle = handle,
		.rc = 2, // Retained by fiber until fiber has been executed, retained by dispatch caller
	};
	
	*internal = (Ygg_Fiber_Internal) {
		.generation = fiber_generation,
		.fiber = fiber,
		.state = Ygg_Fiber_Internal_State_Not_Started,
		
		// Retained by coordinator until fiber has been executed, retained by lazy result
		.rc = 2,
		
		.lazy_result = lazy_result,
		
		// TODO: Pool these or something, don't use free/malloc
		.stack = calloc(YGG_FIBER_STACK_SIZE, 1),
	};
	
	ygg_coordinator_push_fiber(coordinator, handle, priority);
	
	return &internal->lazy_result;
}

Ygg_Lazy_Result* ygg_lazy_result_retain(Ygg_Lazy_Result* result) {
	unsigned int previous = atomic_fetch_add_explicit(&result->rc, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Lazy result was already released");
	return result;
}
void ygg_lazy_result_release(Ygg_Lazy_Result* result) {
	unsigned int previous = atomic_fetch_sub_explicit(&result->rc, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Lazy result over released");
	
	if (previous == 1) {
		// Lazy result has been fully released, we can now release the fiber.
//		Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(result->coordinator, result->fiber_handle);
//		ygg_spinlock_lock(&fiber_internal->spinlock);
		ygg_coordinator_fiber_release(result->coordinator, result->fiber_handle);
//		ygg_spinlock_unlock(&fiber_internal->spinlock);
	}
}
void ygg_lazy_result_unwrap(Ygg_Fiber_Ctx* ctx, Ygg_Lazy_Result* result) {
	Ygg_Fiber_Internal* result_fiber = ygg_coordinator_deref_fiber_handle(ctx->coordinator, result->fiber_handle);
	
	ygg_spinlock_lock(&result_fiber->spinlock);
	Ygg_Fiber_Internal_State state = result_fiber->state;
	if (state != Ygg_Fiber_Internal_State_Complete) {
		// Append current fiber as a dependant on result fiber
		result_fiber->successors[result_fiber->successor_count++] = ctx->fiber_handle;
		ygg_fiber_increment_counter(ctx, 1);
	}
	ygg_spinlock_unlock(&result_fiber->spinlock);
	
	if (state != Ygg_Fiber_Internal_State_Complete) {
		ygg_fiber_wait_for_counter(ctx);
	}
}

// Current fiber functions
void ygg_fiber_increment_counter(Ygg_Fiber_Ctx* ctx, unsigned int n) {
	Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(ctx->coordinator, ctx->fiber_handle);
	atomic_fetch_add_explicit(&internal->counter, n, memory_order_acq_rel);
}
void ygg_fiber_decrement_counter(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(coordinator, handle);
	unsigned int previous = atomic_fetch_sub_explicit(&internal->counter, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Don't underflow");
	
	if (previous == 1) {
		ygg_worker_thread_push_delayed_fiber(internal->owner_thread, handle);
		ygg_semaphore_signal(&coordinator->semaphore);
	}
}
void ygg_fiber_wait_for_counter(Ygg_Fiber_Ctx* ctx) {
	Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(ctx->coordinator, ctx->fiber_handle);
	
	ygg_spinlock_lock(&fiber_internal->spinlock);
	unsigned int counter = fiber_internal->counter;
	ygg_spinlock_unlock(&fiber_internal->spinlock);

	if (counter > 0) {
		// Suspend fiber and return control back to the caller
		ygg_spinlock_lock(&fiber_internal->spinlock);
		fiber_internal->state = Ygg_Fiber_Internal_State_Suspended;
		ygg_spinlock_unlock(&fiber_internal->spinlock);
				
		ygg_cpu_state_store(fiber_internal->resume_state);
		if (fiber_internal->state == Ygg_Fiber_Internal_State_Suspended) {
			ygg_cpu_state_restore(fiber_internal->suspend_state);
		}
	}
}
Ygg_Coordinator* ygg_fiber_coordinator(Ygg_Fiber_Ctx* ctx) {
	return ctx->coordinator;
}
