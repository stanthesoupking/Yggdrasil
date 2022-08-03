
#define YGG_FIBER_STACK_SIZE 128 * 1024
// NOTE: Must be multiple of 2 for arm64

#define YGG_MAXIMUM_FIBERS 1024

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
	
	Ygg_Lazy_Result lazy_result;
	Ygg_Spinlock spinlock;
	
	atomic_uint rc;
	atomic_uint counter;
	
	Ygg_CPU_State resume_state;
	Ygg_CPU_State suspend_state;
	
	// TODO: Allocate elsewhere to decrease struct size
	void* stack;
	
	void* return_stack_pointer;
	
	// TODO: Linked list with pool to support arbitrary array length
	Ygg_Fiber_Handle successors[16];
	unsigned int successor_count;
} Ygg_Fiber_Internal;

typedef struct Ygg_Thread {
	void* stack;
	unsigned int thread_index;
	pthread_t thread;
	Ygg_Coordinator* coordinator;
	
	Ygg_Fiber_Ctx current_ctx;
} Ygg_Thread;

typedef struct Ygg_Coordinator {
	Ygg_Fiber_Internal fiber_storage[YGG_MAXIMUM_FIBERS];
	unsigned int fiber_count;
	
	unsigned int fiber_freelist[YGG_MAXIMUM_FIBERS];
	unsigned int fiber_freelist_length;
	
	void* stack_memory;
	
	struct {
		Ygg_Semaphore semaphore;
		Ygg_Spinlock spinlock;
		Ygg_Fiber_Handle fiber_handles[YGG_MAXIMUM_FIBERS];
		atomic_uint head;
		unsigned int tail;
	} fiber_queue;
	
	Ygg_Thread* threads;
	unsigned int thread_count;
} Ygg_Coordinator;

#define ygg_update_thread_label(...) \
	sprintf(thread_label_postfix, __VA_ARGS__); \
	sprintf(thread_label_full, "%s [%s]", thread_label_prefix, thread_label_postfix); \
	pthread_setname_np(thread_label_full); \

bool ygg_coordinator_pop_fiber_from_queue(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle* handle);
Ygg_Fiber_Internal* ygg_coordinator_deref_fiber_handle(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle);
void ygg_coordinator_fiber_release(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle);
void ygg_fiber_decrement_counter(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle);
void* _ygg_thread(void* data) {
	Ygg_Thread* thread = data;
	Ygg_Coordinator* coordinator = thread->coordinator;
	
	// Thread label for debugging:
	char thread_label_prefix[32];
	char thread_label_postfix[128];
	char thread_label_full[160];
	sprintf(thread_label_prefix, "Yggdrassil %d", thread->thread_index);
		
	ygg_update_thread_label("Idle");
	bool alive = true;
	while (alive) {
		Ygg_Fiber_Handle fiber_handle;
		if (!ygg_coordinator_pop_fiber_from_queue(coordinator, &fiber_handle)) {
			// Wait for semaphore to update and tell us something has changed
			ygg_semaphore_wait(&coordinator->fiber_queue.semaphore);
			continue;
		}
		
		Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(coordinator, fiber_handle);
		
		thread->current_ctx = (Ygg_Fiber_Ctx) {
			.coordinator = coordinator,
			.fiber_handle = fiber_handle,
		};
		
		ygg_update_thread_label("Fiber '%s'", fiber_internal->fiber.label);
		if (fiber_internal->state == Ygg_Fiber_Internal_State_Not_Started) {
			printf("Thread %d: Starting fiber '%s'...\n", thread->thread_index, fiber_internal->fiber.label);
			
			ygg_spinlock_lock(&fiber_internal->spinlock);
			fiber_internal->state = Ygg_Fiber_Internal_State_Running;
			ygg_spinlock_unlock(&fiber_internal->spinlock);
						
			// NOTE: Not sure if == 0 is correct for fibers that resume more than once...
			ygg_cpu_state_store(fiber_internal->suspend_state);
			if (fiber_internal->state == Ygg_Fiber_Internal_State_Running) {
				void* sp = fiber_internal->stack + YGG_FIBER_STACK_SIZE;
				void* ctx = &thread->current_ctx;
				ygg_fiber_boot(sp, fiber_internal->fiber.func, ctx);
								
				printf("Thread %d: Completed fiber '%s'.\n", thread->thread_index, fiber_internal->fiber.label);
				ygg_spinlock_lock(&fiber_internal->spinlock);
				fiber_internal->state = Ygg_Fiber_Internal_State_Complete;
				ygg_spinlock_unlock(&fiber_internal->spinlock);
				ygg_lazy_result_release(&fiber_internal->lazy_result);
				
				// Decrement counters of any waiting fibers
				for (unsigned int successor_index = 0; successor_index < fiber_internal->successor_count; ++successor_index) {
					ygg_fiber_decrement_counter(coordinator, fiber_internal->successors[successor_index]);
				}
				
				ygg_coordinator_fiber_release(coordinator, fiber_handle);
			}
		} else {
			printf("Thread %d: Resuming fiber '%s'...\n", thread->thread_index, fiber_internal->fiber.label);
			fiber_internal->state = Ygg_Fiber_Internal_State_Running;
			ygg_cpu_state_restore(fiber_internal->resume_state);
		}
		printf("Leaving fiber!\n");
		printf("Thread %d: Left fiber\n", thread->thread_index);
		fiber_handle = (Ygg_Fiber_Handle) { };
		fiber_internal = NULL;
		ygg_update_thread_label("Idle");
	}
	
	return NULL;
}

Ygg_Coordinator* ygg_coordinator_new(Ygg_Coordinator_Parameters parameters) {
	Ygg_Coordinator* coordinator = malloc(sizeof(Ygg_Coordinator));
	
	printf("Total stack memory = %.2fMB\n", (YGG_FIBER_STACK_SIZE * YGG_MAXIMUM_FIBERS) / (1024.0f * 1024.0f));
	
	*coordinator = (Ygg_Coordinator) {
		.fiber_count = 0,
		.fiber_freelist_length = 0,
		
		.stack_memory = malloc(YGG_FIBER_STACK_SIZE * YGG_MAXIMUM_FIBERS),
		
		.thread_count = parameters.thread_count,
		
		.fiber_queue = {
			.head = 0,
			.tail = 0,
		},
	};
	
	ygg_semaphore_init(&coordinator->fiber_queue.semaphore);
	
	coordinator->threads = malloc(sizeof(Ygg_Thread) * parameters.thread_count);
	for (unsigned int thread_index = 0; thread_index < parameters.thread_count; ++thread_index) {
		Ygg_Thread* thread = coordinator->threads + thread_index;
		*thread = (Ygg_Thread) {
			.stack = calloc(YGG_FIBER_STACK_SIZE, 1),
			.thread_index = thread_index,
			.coordinator = coordinator,
		};
		pthread_create(&thread->thread, NULL, _ygg_thread, thread);
	}
	
	return coordinator;
}
void ygg_coordinator_destroy(Ygg_Coordinator* coordinator) {
	*coordinator = (Ygg_Coordinator) { };
	free(coordinator);
}

void ygg_coordinator_push_fiber_to_queue(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	ygg_spinlock_lock(&coordinator->fiber_queue.spinlock);
	coordinator->fiber_queue.fiber_handles[(coordinator->fiber_queue.tail++) % YGG_MAXIMUM_FIBERS] = handle;
	ygg_spinlock_unlock(&coordinator->fiber_queue.spinlock);
	
	ygg_semaphore_signal(&coordinator->fiber_queue.semaphore);
}

bool ygg_coordinator_pop_fiber_from_queue(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle* handle) {
	unsigned int head = coordinator->fiber_queue.head;
	if (head == coordinator->fiber_queue.tail) {
		return false;
	}
	
	if (atomic_compare_exchange_strong(&coordinator->fiber_queue.head, &head, head + 1)) {
		*handle = coordinator->fiber_queue.fiber_handles[head % YGG_MAXIMUM_FIBERS];
		return true;
	}

	return false;
}

void ygg_coordinator_fiber_release(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(coordinator, handle);
	unsigned int previous = atomic_fetch_sub_explicit(&internal->rc, 1, memory_order_acquire);
	ygg_assert(previous > 0, "Fiber over released");
	
	if (previous == 1) {
		// Free fiber from system
		
		// TODO: Pool these or something, don't use free/malloc
		free(internal->stack);
		
		unsigned int new_generation = internal->generation + 1;
		*internal = (Ygg_Fiber_Internal) {
			.generation = new_generation,
		};
		coordinator->fiber_freelist[coordinator->fiber_freelist_length++] = handle.index;
		coordinator->fiber_count--;
	}
}

Ygg_Lazy_Result* ygg_coordinator_dispatch(Ygg_Coordinator* coordinator, Ygg_Fiber fiber) {
	unsigned int fiber_index;
	if (coordinator->fiber_freelist_length > 0) {
		fiber_index = coordinator->fiber_freelist[--coordinator->fiber_freelist_length];
	} else {
		ygg_assert(coordinator->fiber_count + 1 < YGG_MAXIMUM_FIBERS, "Maximum fibers exceeded");
		fiber_index = coordinator->fiber_count++;
	}
	
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
	
	ygg_coordinator_push_fiber_to_queue(coordinator, handle);
	
	return &internal->lazy_result;
}

Ygg_Lazy_Result* ygg_lazy_result_retain(Ygg_Lazy_Result* result) {
	unsigned int previous = atomic_fetch_add_explicit(&result->rc, 1, memory_order_acquire);
	ygg_assert(previous > 0, "Lazy result was already released");
	return result;
}
void ygg_lazy_result_release(Ygg_Lazy_Result* result) {
	unsigned int previous = atomic_fetch_sub_explicit(&result->rc, 1, memory_order_acquire);
	ygg_assert(previous > 0, "Lazy result over released");
	
	if (previous == 1) {
		// Lazy result has been fully released, we can now release the fiber.
		Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(result->coordinator, result->fiber_handle);
		ygg_spinlock_lock(&fiber_internal->spinlock);
		ygg_spinlock_unlock(&fiber_internal->spinlock);
		ygg_coordinator_fiber_release(result->coordinator, result->fiber_handle);
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

Ygg_Fiber_Internal* ygg_coordinator_deref_fiber_handle(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	ygg_assert(handle.index < YGG_MAXIMUM_FIBERS, "Fiber index out of bounds");
	
	Ygg_Fiber_Internal* internal = coordinator->fiber_storage + handle.index;
	ygg_assert(internal->generation == handle.generation, "Handle generation is out of date");
	
	return internal;
}

// Current fiber functions
void ygg_fiber_increment_counter(Ygg_Fiber_Ctx* ctx, unsigned int n) {
	Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(ctx->coordinator, ctx->fiber_handle);
	atomic_fetch_add_explicit(&internal->counter, n, memory_order_acquire);
}
void ygg_fiber_decrement_counter(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(coordinator, handle);
	unsigned int previous = atomic_fetch_sub_explicit(&internal->counter, 1, memory_order_acquire);
	ygg_assert(previous > 0, "Don't underflow");
	
	if (previous == 1) {
		ygg_coordinator_push_fiber_to_queue(coordinator, handle);
	}
}
void ygg_fiber_wait_for_counter(Ygg_Fiber_Ctx* ctx) {
	Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(ctx->coordinator, ctx->fiber_handle);
	
	ygg_spinlock_lock(&fiber_internal->spinlock);
	unsigned int counter = fiber_internal->counter;
	ygg_spinlock_unlock(&fiber_internal->spinlock);

	if (counter > 0) {
		printf("Suspending\n");
		// Suspend fiber and return control back to the caller
		ygg_spinlock_lock(&fiber_internal->spinlock);
		fiber_internal->state = Ygg_Fiber_Internal_State_Suspended;
		ygg_spinlock_unlock(&fiber_internal->spinlock);
		
		// TODO: Fiber should be pushed to thread's delayed fiber queue, a fiber can only execute on it's initial thread
		
		ygg_cpu_state_store(fiber_internal->resume_state);
		if (fiber_internal->state == Ygg_Fiber_Internal_State_Suspended) {
			ygg_cpu_state_restore(fiber_internal->suspend_state);
		}
		
		printf("we're back!\n");
	}
}
Ygg_Coordinator* ygg_fiber_coordinator(Ygg_Fiber_Ctx* ctx) {
	return ctx->coordinator;
}
