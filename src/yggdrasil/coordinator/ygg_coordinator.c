
#define YGG_FIBER_STACK_SIZE 128 * 1024
#define YGG_MAXIMUM_FIBERS 1024
#define YGG_QUEUE_SIZE 1024

typedef struct Ygg_Fiber_Ctx {
	Ygg_Coordinator* coordinator;
	Ygg_Fiber_Handle fiber_handle;
} Ygg_Fiber_Ctx;

typedef struct Ygg_Future {
	Ygg_Coordinator* coordinator;
	Ygg_Spinlock spinlock;
	Ygg_Fiber_Handle fiber_handle;
	atomic_uint rc;
	bool fulfilled;
	
	// TODO: Linked list with pool to support arbitrary array length
	Ygg_Fiber_Handle waiting_fibers[16];
	unsigned int waiting_fiber_count;
} Ygg_Future;
ygg_pool(Ygg_Future, Ygg_Future_Pool, ygg_future_pool);

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
	
	Ygg_Future* future; // owned(retained)
	Ygg_Spinlock spinlock;
	
	atomic_uint rc;
	atomic_uint counter;
	
	Ygg_CPU_State resume_state;
	Ygg_CPU_State suspend_state;
	
	// TODO: Allocate elsewhere to decrease struct size
	void* stack;
} Ygg_Fiber_Internal;

typedef struct Ygg_Coordinator {
	Ygg_Fiber_Internal fiber_storage[YGG_MAXIMUM_FIBERS];
	unsigned int fiber_count;
	
	Ygg_Spinlock future_pool_spinlock;
	Ygg_Future_Pool future_pool;
	
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
	
	ygg_future_pool_init(&coordinator->future_pool, YGG_MAXIMUM_FIBERS);
	
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

Ygg_Future* ygg_coordinator_dispatch(Ygg_Coordinator* coordinator, Ygg_Fiber fiber, Ygg_Priority priority) {
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
	
	ygg_spinlock_lock(&coordinator->future_pool_spinlock);
	Ygg_Future* future = ygg_future_pool_acquire(&coordinator->future_pool);
	ygg_spinlock_unlock(&coordinator->future_pool_spinlock);
	*future = (Ygg_Future) {
		.coordinator = coordinator,
		.fiber_handle = handle,
		
		// Retained by fiber until fiber has been executed, retained by dispatch caller
		.rc = 2,
	};
	
	*internal = (Ygg_Fiber_Internal) {
		.generation = fiber_generation,
		.fiber = fiber,
		.state = Ygg_Fiber_Internal_State_Not_Started,
		
		// Retained by coordinator until fiber has been executed
		.rc = 1,
		
		.future = future,
		
		// TODO: Pool these or something, don't use free/malloc
		.stack = calloc(YGG_FIBER_STACK_SIZE, 1),
	};
	
	ygg_coordinator_push_fiber(coordinator, handle, priority);
	
	return future;
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

// Futures
Ygg_Future* ygg_future_retain(Ygg_Future* future) {
	unsigned int previous = atomic_fetch_add_explicit(&future->rc, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Future was already released");
	return future;
}
void ygg_future_release(Ygg_Future* future) {
	unsigned int previous = atomic_fetch_sub_explicit(&future->rc, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Future over released");
	
	if (previous == 1) {
		Ygg_Coordinator* coordinator = future->coordinator;
		ygg_spinlock_lock(&coordinator->future_pool_spinlock);
		ygg_future_pool_release(&coordinator->future_pool, future);
		ygg_spinlock_unlock(&coordinator->future_pool_spinlock);
	}
}
void ygg_future_wait(Ygg_Future* future, Ygg_Fiber_Ctx* current_context) {
	ygg_spinlock_lock(&future->spinlock);
	if (future->fulfilled) {
		ygg_spinlock_unlock(&future->spinlock);
		return;
	} else {
		ygg_fiber_increment_counter(current_context, 1);
		future->waiting_fibers[future->waiting_fiber_count++] = current_context->fiber_handle;
		ygg_spinlock_unlock(&future->spinlock);
		ygg_fiber_wait_for_counter(current_context);
	}
}
void ygg_future_fulfill(Ygg_Future* future) {
	ygg_spinlock_lock(&future->spinlock);
	ygg_assert(!future->fulfilled, "Future has already been fulfilled.");
	future->fulfilled = true;
	for (unsigned int i = 0; i < future->waiting_fiber_count; ++i) {
		ygg_fiber_decrement_counter(future->coordinator, future->waiting_fibers[i]);
	}
	ygg_spinlock_unlock(&future->spinlock);
}
