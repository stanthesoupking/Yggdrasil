
#define YGG_FIBER_STACK_SIZE 8 * 1024 * 1024 // 8MB
#define YGG_MAXIMUM_FIBERS 1024
#define YGG_MAXIMUM_INPUT_LENGTH 64
#define YGG_MAXIMUM_BARRIERS 1024

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

typedef struct Ygg_Context_Node Ygg_Context_Node;
typedef struct Ygg_Context_Node {
	Ygg_Context* context;
	Ygg_Context_Node* next;
} Ygg_Context_Node;
ygg_pool(Ygg_Context_Node, Ygg_Context_Node_Pool, ygg_context_node_pool);

typedef struct Ygg_Counter {
	unsigned int generation;
	Ygg_Coordinator* coordinator;
	Ygg_Spinlock spinlock;
	atomic_uint rc;
	atomic_uint counter;
	
	Ygg_Context_Node* waiting;
} Ygg_Counter;
ygg_growable_pool(Ygg_Counter, Ygg_Counter_Handle, Ygg_Counter_Pool, ygg_counter_pool, 64);

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
	
	Ygg_Spinlock spinlock;
	
	atomic_uint rc;
	
	Ygg_CPU_State resume_state;
	Ygg_CPU_State suspend_state;
	
	Ygg_Counter_Handle registered_counters[16];
	unsigned int registered_counter_count;
	
	void* stack;
} Ygg_Fiber_Internal;
ygg_growable_pool(Ygg_Fiber_Internal, Ygg_Fiber_Handle, Ygg_Fiber_Internal_Pool, ygg_fiber_internal_pool, 64);

typedef struct Ygg_Coordinator {
	Ygg_Spinlock fiber_pool_spinlock;
	Ygg_Fiber_Internal_Pool fiber_pool;

	Ygg_Spinlock context_node_pool_spinlock;
	Ygg_Context_Node_Pool context_node_pool;
	
	Ygg_Spinlock counter_pool_spinlock;
	Ygg_Counter_Pool counter_pool;
	
	Ygg_Fiber_Queue fiber_queues[YGG_PRIORITY_COUNT];
	
	Ygg_Worker_Thread** worker_threads;
	unsigned int worker_thread_count;
	
	Ygg_Context blocking_context;
	
	bool shutting_down;
} Ygg_Coordinator;

Ygg_Coordinator* ygg_coordinator_new(Ygg_Coordinator_Parameters parameters) {
	Ygg_Coordinator* coordinator = calloc(sizeof(Ygg_Coordinator), 1);
		
	*coordinator = (Ygg_Coordinator) {
		.worker_thread_count = parameters.thread_count,
		.blocking_context = (Ygg_Context) {
			.coordinator = coordinator,
			.kind = Ygg_Context_Kind_Blocking,
		},
	};
	
	ygg_fiber_internal_pool_init(&coordinator->fiber_pool, 1);
	ygg_context_node_pool_init(&coordinator->context_node_pool, YGG_MAXIMUM_FIBERS);
	ygg_counter_pool_init(&coordinator->counter_pool, 1);
		
	for (unsigned int queue_index = 0; queue_index < YGG_PRIORITY_COUNT; ++queue_index) {
		ygg_fiber_queue_init(coordinator->fiber_queues + queue_index, 64);
	}
	
	coordinator->worker_threads = malloc(sizeof(Ygg_Worker_Thread*) * parameters.thread_count);
	for (unsigned int thread_index = 0; thread_index < parameters.thread_count; ++thread_index) {
		coordinator->worker_threads[thread_index] = ygg_worker_thread_new(coordinator, thread_index);
		ygg_worker_thread_start(coordinator->worker_threads[thread_index]);
	}
	
	return coordinator;
}
void ygg_coordinator_destroy(Ygg_Coordinator* coordinator) {
	coordinator->shutting_down = true;
	
	// Wait for all workers to shutdown
	for (unsigned int worker_index = 0; worker_index < coordinator->worker_thread_count; worker_index++) {
		ygg_semaphore_signal(ygg_worker_thread_semaphore(coordinator->worker_threads[worker_index]));
		ygg_worker_thread_join(coordinator->worker_threads[worker_index]);
		ygg_worker_thread_destroy(coordinator->worker_threads[worker_index]);
	}
	free(coordinator->worker_threads);
	
	ygg_counter_pool_deinit(&coordinator->counter_pool);
	ygg_fiber_internal_pool_deinit(&coordinator->fiber_pool);
	ygg_context_node_pool_deinit(&coordinator->context_node_pool);
	
	for (unsigned int queue_index = 0; queue_index < YGG_PRIORITY_COUNT; ++queue_index) {
		ygg_fiber_queue_deinit(coordinator->fiber_queues + queue_index);
	}
	*coordinator = (Ygg_Coordinator) { };
	free(coordinator);
}
bool ygg_coordinator_has_nonempty_queue(Ygg_Coordinator* coordinator) {
	for (unsigned int i = 0; i < YGG_PRIORITY_COUNT; ++i) {
		if (coordinator->fiber_queues[i].count > 0) { return true; }
	}
	return false;
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
	ygg_semaphore_deinit(&blocking_context->semaphore);
	free(blocking_context);
}
Ygg_Coordinator* ygg_context_coordinator(Ygg_Context* context) {
	return context->coordinator;
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

void ygg_coordinator_fiber_release(Ygg_Coordinator* coordinator, Ygg_Fiber_Handle handle) {
	ygg_spinlock_lock(&coordinator->fiber_pool_spinlock);
	Ygg_Fiber_Internal* internal = ygg_fiber_internal_pool_deref(&coordinator->fiber_pool, handle);
	ygg_spinlock_unlock(&coordinator->fiber_pool_spinlock);
	
	unsigned int previous = atomic_fetch_sub_explicit(&internal->rc, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Fiber over released");
	
	if (previous == 1) {
		// TODO: Pool these or something, don't use free/malloc
		ygg_virtual_free(internal->stack, YGG_FIBER_STACK_SIZE);
		
		ygg_spinlock_lock(&coordinator->fiber_pool_spinlock);
		ygg_fiber_internal_pool_release(&coordinator->fiber_pool, handle);
		ygg_spinlock_unlock(&coordinator->fiber_pool_spinlock);
	}
}

Ygg_Fiber_Handle ygg_dispatch_generic_async(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output_ptr) {
	ygg_assert(input_length < YGG_MAXIMUM_INPUT_LENGTH, "Maximum input length of %d bytes exceeded.", YGG_MAXIMUM_INPUT_LENGTH);
	
	Ygg_Coordinator* coordinator = context->coordinator;
	
	ygg_spinlock_lock(&coordinator->fiber_pool_spinlock);
	Ygg_Fiber_Handle handle = ygg_fiber_internal_pool_acquire(&coordinator->fiber_pool);
	Ygg_Fiber_Internal* internal = ygg_fiber_internal_pool_deref(&coordinator->fiber_pool, handle);
	ygg_spinlock_unlock(&coordinator->fiber_pool_spinlock);
	
	*internal = (Ygg_Fiber_Internal) {
		.generation = handle.generation,
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
				
		// TODO: Pool these or something, don't use free/malloc
		.stack = ygg_virtual_alloc(YGG_FIBER_STACK_SIZE),
	};
	
	if (input != NULL) {
		memcpy(internal->input, input, input_length);
	}
	
	ygg_coordinator_push_fiber(coordinator, handle, priority);
	
	return handle;
}

void ygg_dispatch_generic_sync(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output_ptr) {
	Ygg_Counter_Handle counter = ygg_counter_new(context->coordinator);
	Ygg_Fiber_Handle fiber_handle = ygg_dispatch_generic_async(context, fiber, priority, input, input_length, output_ptr);
	ygg_counter_await_completion(counter, fiber_handle);
	ygg_counter_wait(counter, context);
	ygg_counter_release(counter);
}

// Current fiber functions
void ygg_context_resume(Ygg_Context* context) {
	switch (context->kind) {
		case Ygg_Context_Kind_Fiber: {
			Ygg_Coordinator* coordinator = context->coordinator;
			ygg_spinlock_lock(&coordinator->fiber_pool_spinlock);
			Ygg_Fiber_Internal* internal = ygg_fiber_internal_pool_deref(&coordinator->fiber_pool, context->fiber_handle);
			ygg_spinlock_unlock(&coordinator->fiber_pool_spinlock);
			
			ygg_spinlock_lock(&internal->spinlock);
			if (internal->state == Ygg_Fiber_Internal_State_Suspended) {
				ygg_worker_thread_push_delayed_fiber(internal->owner_thread, context->fiber_handle);
				ygg_semaphore_signal(ygg_worker_thread_semaphore(internal->owner_thread));
			}
			ygg_spinlock_unlock(&internal->spinlock);
		} break;
			
		case Ygg_Context_Kind_Blocking: {
			ygg_semaphore_signal(&context->semaphore);
		} break;
	}
}
void ygg_context_suspend(Ygg_Context* context) {
	switch (context->kind) {
		case Ygg_Context_Kind_Fiber: {
			Ygg_Coordinator* coordinator = context->coordinator;
			ygg_spinlock_lock(&coordinator->fiber_pool_spinlock);
			Ygg_Fiber_Internal* internal = ygg_fiber_internal_pool_deref(&coordinator->fiber_pool, context->fiber_handle);
			ygg_spinlock_unlock(&coordinator->fiber_pool_spinlock);
						
			// Suspend fiber and return control back to the caller
			ygg_spinlock_lock(&internal->spinlock);
			internal->state = Ygg_Fiber_Internal_State_Suspended;
			ygg_spinlock_unlock(&internal->spinlock);
			
			ygg_cpu_state_store(internal->resume_state);
			if (internal->state == Ygg_Fiber_Internal_State_Suspended) {
				ygg_cpu_state_restore(internal->suspend_state);
			}
		} break;
		case Ygg_Context_Kind_Blocking: {
			ygg_semaphore_wait(&context->semaphore);
		} break;
	}
}

// MARK: Counter
Ygg_Counter_Handle ygg_counter_new(Ygg_Coordinator* coordinator) {
	ygg_spinlock_lock(&coordinator->counter_pool_spinlock);
	Ygg_Counter_Handle handle = ygg_counter_pool_acquire(&coordinator->counter_pool);
	Ygg_Counter* counter = ygg_counter_pool_deref(&coordinator->counter_pool, handle);
	ygg_spinlock_unlock(&coordinator->counter_pool_spinlock);
	
	handle.coordinator = coordinator;
	
	ygg_spinlock_init(&counter->spinlock);
	counter->rc = 1;
	counter->waiting = NULL;
	counter->counter = 0;
	counter->coordinator = coordinator;
	
	return handle;
}
void ygg_counter_retain(Ygg_Counter_Handle counter) {
	ygg_spinlock_lock(&counter.coordinator->counter_pool_spinlock);
	Ygg_Counter* counter_internal = ygg_counter_pool_deref(&counter.coordinator->counter_pool, counter);
	ygg_spinlock_unlock(&counter.coordinator->counter_pool_spinlock);
	atomic_fetch_add_explicit(&counter_internal->rc, 1, memory_order_acq_rel);
}
void ygg_counter_release(Ygg_Counter_Handle counter) {
	ygg_spinlock_lock(&counter.coordinator->counter_pool_spinlock);
	Ygg_Counter* counter_internal = ygg_counter_pool_deref(&counter.coordinator->counter_pool, counter);
	ygg_spinlock_unlock(&counter.coordinator->counter_pool_spinlock);
	unsigned int prev = atomic_fetch_sub_explicit(&counter_internal->rc, 1, memory_order_acq_rel);
	ygg_assert(prev > 0, "Don't underflow");
	
	if (prev == 1) {
		ygg_spinlock_lock(&counter.coordinator->counter_pool_spinlock);
		ygg_counter_pool_release(&counter.coordinator->counter_pool, counter);
		ygg_spinlock_unlock(&counter.coordinator->counter_pool_spinlock);
	}
}

void ygg_counter_increment(Ygg_Counter_Handle counter, unsigned int n) {
	ygg_spinlock_lock(&counter.coordinator->counter_pool_spinlock);
	Ygg_Counter* counter_internal = ygg_counter_pool_deref(&counter.coordinator->counter_pool, counter);
	ygg_spinlock_unlock(&counter.coordinator->counter_pool_spinlock);
	
	ygg_spinlock_lock(&counter_internal->spinlock);
	atomic_fetch_add_explicit(&counter_internal->counter, n, memory_order_acq_rel);
	ygg_spinlock_unlock(&counter_internal->spinlock);
}
void ygg_counter_decrement(Ygg_Counter_Handle counter, unsigned int n) {
	ygg_spinlock_lock(&counter.coordinator->counter_pool_spinlock);
	Ygg_Counter* counter_internal = ygg_counter_pool_deref(&counter.coordinator->counter_pool, counter);
	ygg_spinlock_unlock(&counter.coordinator->counter_pool_spinlock);
	
	ygg_spinlock_lock(&counter_internal->spinlock);
	unsigned int prev = atomic_fetch_sub_explicit(&counter_internal->counter, n, memory_order_acq_rel);
	ygg_assert(prev > 0, "Don't underflow");
	if (prev == 1) {
		// Wake up blocked fibers
		Ygg_Context_Node* entry = counter_internal->waiting;
		while (entry != NULL) {
			Ygg_Coordinator* coordinator = entry->context->coordinator;
			ygg_context_resume(entry->context);
			Ygg_Context_Node* next = entry->next;
			
			ygg_spinlock_lock(&coordinator->context_node_pool_spinlock);
			ygg_context_node_pool_release(&coordinator->context_node_pool, entry);
			ygg_spinlock_unlock(&coordinator->context_node_pool_spinlock);
			
			entry = next;
		}
	}
	ygg_spinlock_unlock(&counter_internal->spinlock);
}
void ygg_counter_await_completion(Ygg_Counter_Handle counter, Ygg_Fiber_Handle fiber_handle) {
	Ygg_Coordinator* coordinator = counter.coordinator;
	
	ygg_spinlock_lock(&coordinator->fiber_pool_spinlock);
	Ygg_Fiber_Internal* internal = ygg_fiber_internal_pool_deref(&coordinator->fiber_pool, fiber_handle);
	if (internal == NULL) {
		ygg_spinlock_unlock(&coordinator->fiber_pool_spinlock);
	} else {
		ygg_spinlock_lock(&internal->spinlock);

		if (internal->state == Ygg_Fiber_Internal_State_Complete) {
			ygg_spinlock_unlock(&internal->spinlock);
		} else {
			ygg_counter_increment(counter, 1);
			internal->registered_counters[internal->registered_counter_count++] = counter;
			ygg_counter_retain(counter); // retained by fiber
			ygg_spinlock_unlock(&internal->spinlock);
		}
		
		ygg_spinlock_unlock(&coordinator->fiber_pool_spinlock);
	}
}

void ygg_counter_wait(Ygg_Counter_Handle counter, Ygg_Context* context) {
	Ygg_Coordinator* coordinator = counter.coordinator;

	ygg_spinlock_lock(&coordinator->counter_pool_spinlock);
	Ygg_Counter* counter_internal = ygg_counter_pool_deref(&coordinator->counter_pool, counter);
	ygg_spinlock_unlock(&coordinator->counter_pool_spinlock);
	
	ygg_spinlock_lock(&counter_internal->spinlock);
	if (counter_internal->counter == 0) {
		ygg_spinlock_unlock(&counter_internal->spinlock);
		return;
	}
	
	ygg_spinlock_lock(&coordinator->context_node_pool_spinlock);
	Ygg_Context_Node* entry = ygg_context_node_pool_acquire(&coordinator->context_node_pool);
	ygg_spinlock_unlock(&coordinator->context_node_pool_spinlock);
	
	*entry = (Ygg_Context_Node) {
		.context = context,
	};
	if (counter_internal->waiting == NULL) {
		counter_internal->waiting = entry;
	} else {
		entry->next = counter_internal->waiting;
		counter_internal->waiting = entry;
	}
	
	ygg_spinlock_unlock(&counter_internal->spinlock);
	ygg_context_suspend(context);
}
