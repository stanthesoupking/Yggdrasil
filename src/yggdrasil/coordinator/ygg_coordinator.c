
#define YGG_FIBER_STACK_SIZE 128 * 1024
#define YGG_MAXIMUM_FIBERS 1024
#define YGG_MAXIMUM_INPUT_LENGTH 64
#define YGG_QUEUE_SIZE 1024

typedef enum Ygg_Context_Kind {
	Ygg_Context_Kind_Fiber,
	Ygg_Context_Kind_Blocking,
} Ygg_Context_Kind;

typedef struct Ygg_Context {
	Ygg_Context_Kind kind;
	Ygg_Coordinator* coordinator;
	
	// Ygg_Context_Kind_Fiber
	Ygg_Fiber_Handle fiber_handle;
	
	// Ygg_Context_Kind_Blocking
	Ygg_Spinlock spinlock;
	atomic_uint counter;
	Ygg_Semaphore semaphore;
} Ygg_Context;

typedef struct Ygg_Future_Waiting_Entry Ygg_Future_Waiting_Entry;
typedef struct Ygg_Future_Waiting_Entry {
	Ygg_Context* context;
	Ygg_Future_Waiting_Entry* next;
} Ygg_Future_Waiting_Entry;
ygg_pool(Ygg_Future_Waiting_Entry, Ygg_Future_Waiting_Entry_Pool, ygg_future_waiting_entry_pool);

typedef struct Ygg_Future {
	Ygg_Coordinator* coordinator;
	Ygg_Spinlock spinlock;
	Ygg_Fiber_Handle fiber_handle;
	atomic_uint rc;
	bool fulfilled;
		
	Ygg_Future_Waiting_Entry* waiting;
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
	
	unsigned char input[YGG_MAXIMUM_INPUT_LENGTH];
	void* output;
	
	Ygg_Context context;
		
	// Valid after the fiber has been started
	Ygg_Worker_Thread* owner_thread;
	
	Ygg_Future* future; // owned(retained)
	Ygg_Spinlock spinlock;
	
	atomic_uint rc;
	atomic_uint counter;
	
	Ygg_CPU_State resume_state;
	Ygg_CPU_State suspend_state;
	
	void* stack;
} Ygg_Fiber_Internal;

typedef struct Ygg_Coordinator {
	Ygg_Fiber_Internal fiber_storage[YGG_MAXIMUM_FIBERS];
	unsigned int fiber_count;
	
	Ygg_Spinlock future_pool_spinlock; // guards access to `future_pool` and `future_waiting_entry_pool`
	Ygg_Future_Pool future_pool;
	Ygg_Future_Waiting_Entry_Pool future_waiting_entry_pool;
	
	unsigned int fiber_freelist[YGG_MAXIMUM_FIBERS];
	unsigned int fiber_freelist_length;
	Ygg_Spinlock fiber_freelist_spinlock;
		
	Ygg_Fiber_Queue fiber_queues[YGG_PRIORITY_COUNT];
	
	Ygg_Worker_Thread** worker_threads;
	unsigned int worker_thread_count;
	
	Ygg_Context blocking_context;
} Ygg_Coordinator;

Ygg_Coordinator* ygg_coordinator_new(Ygg_Coordinator_Parameters parameters) {
	Ygg_Coordinator* coordinator = calloc(sizeof(Ygg_Coordinator), 1);
		
	*coordinator = (Ygg_Coordinator) {
		.fiber_count = 0,
		.fiber_freelist_length = YGG_MAXIMUM_FIBERS,
				
		.worker_thread_count = parameters.thread_count,
		.blocking_context = (Ygg_Context) {
			.coordinator = coordinator,
			.kind = Ygg_Context_Kind_Blocking,
		},
	};
	
	ygg_future_pool_init(&coordinator->future_pool, YGG_MAXIMUM_FIBERS);
	ygg_future_waiting_entry_pool_init(&coordinator->future_waiting_entry_pool, YGG_MAXIMUM_FIBERS);
	
	for (unsigned int fiber_index = 0; fiber_index < YGG_MAXIMUM_FIBERS; ++fiber_index) {
		coordinator->fiber_freelist[YGG_MAXIMUM_FIBERS - fiber_index - 1] = fiber_index;
	}
		
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

Ygg_Context* ygg_blocking_context_new(Ygg_Coordinator* coordinator) {
	Ygg_Context* blocking_context = malloc(sizeof(Ygg_Context));
	*blocking_context = (Ygg_Context) {
		.kind = Ygg_Context_Kind_Blocking,
		.coordinator = coordinator,
		.counter = 0,
	};
	ygg_semaphore_init(&blocking_context->semaphore);
	return blocking_context;
}
void ygg_blocking_context_destroy(Ygg_Context* blocking_context) {
	ygg_assert(blocking_context->kind == Ygg_Context_Kind_Blocking, "");
}

ygg_internal void ygg_coordinator_push_fiber(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle, Ygg_Priority priority) {
	ygg_fiber_queue_push(coordinator->fiber_queues + priority, handle);
	for (unsigned int worker_index = 0; worker_index < coordinator->worker_thread_count; ++worker_index) {
		Ygg_Semaphore* worker_semaphore = ygg_worker_thread_semaphore(coordinator->worker_threads[worker_index]);
		ygg_semaphore_signal(worker_semaphore);
	}
}
ygg_internal bool ygg_coordinator_pop_fiber(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle* handle) {
	for (int queue_index = YGG_PRIORITY_COUNT - 1; queue_index >= 0; --queue_index) {
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

Ygg_Future* ygg_dispatch_generic_async(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output_ptr) {
	ygg_assert(input_length < YGG_MAXIMUM_INPUT_LENGTH, "Maximum input length of %d bytes exceeded.", YGG_MAXIMUM_INPUT_LENGTH);
	
	Ygg_Coordinator* coordinator = context->coordinator;
	
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
		
		.output = output_ptr,
		
		.context = (Ygg_Context) {
			.kind = Ygg_Context_Kind_Fiber,
			.coordinator = coordinator,
			.fiber_handle = handle,
		},
		
		// Retained by coordinator until fiber has been executed
		.rc = 1,
		
		.future = future,
		
		// TODO: Pool these or something, don't use free/malloc
		.stack = calloc(YGG_FIBER_STACK_SIZE, 1),
	};
	
	if (input != NULL) {
		memcpy(internal->input, input, input_length);
	}
	
	ygg_coordinator_push_fiber(coordinator, handle, priority);
	
	return future;
}

void ygg_dispatch_generic_sync(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output_ptr) {
	Ygg_Future* future = ygg_dispatch_generic_async(context, fiber, priority, input, input_length, output_ptr);
	ygg_await(context, future);
	ygg_future_release(future);
}

// Current fiber functions
void ygg_increment_counter(Ygg_Context* ctx, unsigned int n) {
	switch (ctx->kind) {
		case Ygg_Context_Kind_Fiber: {
			Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(ctx->coordinator, ctx->fiber_handle);
			atomic_fetch_add_explicit(&internal->counter, n, memory_order_acq_rel);
		} break;
			
		case Ygg_Context_Kind_Blocking: {
			atomic_fetch_add_explicit(&ctx->counter, 1, memory_order_acq_rel);
		} break;
	}
}
void ygg_fiber_decrement_counter(Ygg_Context* ctx, unsigned int n) {
	switch (ctx->kind) {
		case Ygg_Context_Kind_Fiber: {
			Ygg_Fiber_Internal* internal = ygg_coordinator_deref_fiber_handle(ctx->coordinator, ctx->fiber_handle);
			
			ygg_spinlock_lock(&internal->spinlock);
			unsigned int previous = atomic_fetch_sub_explicit(&internal->counter, 1, memory_order_acq_rel);
			ygg_assert(previous > 0, "Don't underflow");
			
			if ((previous == 1) && (internal->state == Ygg_Fiber_Internal_State_Suspended)) {
				ygg_worker_thread_push_delayed_fiber(internal->owner_thread, ctx->fiber_handle);
				ygg_semaphore_signal(ygg_worker_thread_semaphore(internal->owner_thread));
			}
			ygg_spinlock_unlock(&internal->spinlock);
		} break;
			
		case Ygg_Context_Kind_Blocking: {
			unsigned int prev = atomic_fetch_sub_explicit(&ctx->counter, 1, memory_order_acq_rel);
			ygg_assert(prev > 0, "Don't underflow");
			if (prev == 1) {
				ygg_semaphore_signal(&ctx->semaphore);
			}
		} break;
	}
}
void ygg_wait_for_counter(Ygg_Context* ctx) {
	switch (ctx->kind) {
		case Ygg_Context_Kind_Fiber: {
			Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(ctx->coordinator, ctx->fiber_handle);
			
			ygg_spinlock_lock(&fiber_internal->spinlock);
			if (fiber_internal->counter > 0) {
				// Suspend fiber and return control back to the caller
				fiber_internal->state = Ygg_Fiber_Internal_State_Suspended;
				ygg_spinlock_unlock(&fiber_internal->spinlock);
				ygg_cpu_state_store(fiber_internal->resume_state);
				if (fiber_internal->state == Ygg_Fiber_Internal_State_Suspended) {
					ygg_cpu_state_restore(fiber_internal->suspend_state);
				}
			} else {
				ygg_spinlock_unlock(&fiber_internal->spinlock);
			}
		} break;
			
		case Ygg_Context_Kind_Blocking: {
			ygg_semaphore_wait(&ctx->semaphore);
		} break;
	}
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
		Ygg_Future_Waiting_Entry* entry = future->waiting;
		while (entry != NULL) {
			Ygg_Future_Waiting_Entry* next = entry->next;
			ygg_future_waiting_entry_pool_release(&coordinator->future_waiting_entry_pool, entry);
			entry = next;
		}
		ygg_future_pool_release(&coordinator->future_pool, future);
		ygg_spinlock_unlock(&coordinator->future_pool_spinlock);
	}
}

void ygg_await(Ygg_Context* context, Ygg_Future* future) {
	ygg_spinlock_lock(&future->spinlock);
	if (future->fulfilled) {
		ygg_spinlock_unlock(&future->spinlock);
		return;
	} else {
		ygg_increment_counter(context, 1);
		
		Ygg_Coordinator* coordinator = future->coordinator;
		
		ygg_spinlock_lock(&coordinator->future_pool_spinlock);
		Ygg_Future_Waiting_Entry* entry = ygg_future_waiting_entry_pool_acquire(&coordinator->future_waiting_entry_pool);
		ygg_spinlock_unlock(&coordinator->future_pool_spinlock);

		*entry = (Ygg_Future_Waiting_Entry) {
			.context = context,
		};
		if (future->waiting == NULL) {
			future->waiting = entry;
		} else {
			entry->next = future->waiting;
			future->waiting = entry;
		}
		
		ygg_spinlock_unlock(&future->spinlock);
		ygg_wait_for_counter(context);
	}
}

void ygg_future_fulfill(Ygg_Future* future) {
	ygg_spinlock_lock(&future->spinlock);
	ygg_assert(!future->fulfilled, "Future has already been fulfilled.");
	future->fulfilled = true;
	
	Ygg_Future_Waiting_Entry* entry = future->waiting;
	while (entry != NULL) {
		ygg_fiber_decrement_counter(entry->context, 1);
		entry = entry->next;
	}
	
	ygg_spinlock_unlock(&future->spinlock);
}


// tmp
typedef struct Ygg_Future_Generic {
	
} Ygg_Future_Generic;
void ygg_future_generic_retain(Ygg_Future_Generic* generic) {
	
}
void ygg_future_generic_release(Ygg_Future_Generic* generic) {
	
}
const void* ygg_future_generic_unwrap(Ygg_Future_Generic* generic) {
	return NULL;
}
