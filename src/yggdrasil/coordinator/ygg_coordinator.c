
#define YGG_FIBER_STACK_SIZE 128 * 1024 // 128KB
#define YGG_MAXIMUM_INPUT_LENGTH 64

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

typedef struct Ygg_Counter_Internal {
	unsigned int generation;
	Ygg_Coordinator* coordinator;
	Ygg_Spinlock spinlock;
	atomic_uint rc;
	atomic_uint counter;
	
	Ygg_Context_Node* waiting;
} Ygg_Counter_Internal;

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

typedef struct Ygg_Coordinator {
	Ygg_Spinlock fiber_freelist_spinlock;
	unsigned int* fiber_freelist;
	unsigned int fiber_freelist_length;
	Ygg_Fiber_Internal* fibers;
	unsigned int maximum_fibers;
	
	Ygg_Spinlock counter_freelist_spinlock;
	unsigned int* counter_freelist;
	unsigned int counter_freelist_length;
	Ygg_Counter_Internal* counters;
	unsigned int maximum_counters;

	Ygg_Spinlock context_node_pool_spinlock;
	Ygg_Context_Node_Pool context_node_pool;
	
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
		.fiber_freelist = malloc(sizeof(unsigned int) * parameters.maximum_fibers),
		.fibers = malloc(sizeof(Ygg_Fiber_Internal) * parameters.maximum_fibers),
		.counter_freelist = malloc(sizeof(unsigned int) * parameters.maximum_counters),
		.counters = malloc(sizeof(Ygg_Counter_Internal) * parameters.maximum_counters),
	};
	
	coordinator->fiber_freelist_length = parameters.maximum_fibers;
	for (unsigned int i = 0; i < parameters.maximum_fibers; ++i) {
		coordinator->fiber_freelist[i] = parameters.maximum_fibers - i - 1;
	}
	
	coordinator->counter_freelist_length = parameters.maximum_counters;
	for (unsigned int i = 0; i < parameters.maximum_counters; ++i) {
		coordinator->counter_freelist[i] = parameters.maximum_counters - i - 1;
	}
	
	ygg_context_node_pool_init(&coordinator->context_node_pool, parameters.maximum_fibers);
		
	for (unsigned int queue_index = 0; queue_index < YGG_PRIORITY_COUNT; ++queue_index) {
		ygg_fiber_queue_init(coordinator->fiber_queues + queue_index, parameters.queue_capacity);
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
	
	free(coordinator->fiber_freelist);
	free(coordinator->fibers);
	free(coordinator->counter_freelist);
	free(coordinator->counters);
	
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
	Ygg_Fiber_Internal* internal = coordinator->fibers + handle.index;
	if (internal->generation != handle.generation) {
		return;
	}
	
	unsigned int previous = atomic_fetch_sub_explicit(&internal->rc, 1, memory_order_acq_rel);
	ygg_assert(previous > 0, "Fiber over released");
	
	if (previous == 1) {
		// TODO: Pool these or something, don't use free/malloc
		free(internal->stack);
		
		++internal->generation;
		ygg_spinlock_lock(&coordinator->fiber_freelist_spinlock);
		coordinator->fiber_freelist[coordinator->fiber_freelist_length++] = handle.index;
		ygg_spinlock_unlock(&coordinator->fiber_freelist_spinlock);
	}
}

Ygg_Fiber_Handle ygg_dispatch_generic_async(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output_ptr) {
	ygg_assert(input_length < YGG_MAXIMUM_INPUT_LENGTH, "Maximum input length of %d bytes exceeded.", YGG_MAXIMUM_INPUT_LENGTH);
	
	Ygg_Coordinator* coordinator = context->coordinator;
	
	ygg_spinlock_lock(&coordinator->fiber_freelist_spinlock);
	ygg_assert(coordinator->fiber_freelist_length > 0, "Fiber limit exceeded");
	unsigned int index = coordinator->fiber_freelist[--coordinator->fiber_freelist_length];
	ygg_spinlock_unlock(&coordinator->fiber_freelist_spinlock);
	
	Ygg_Fiber_Internal* internal = coordinator->fibers + index;
	Ygg_Fiber_Handle handle = (Ygg_Fiber_Handle) {
		.index = index,
		.generation = ++internal->generation,
	};
	
	internal->fiber = fiber;
	internal->state = Ygg_Fiber_Internal_State_Not_Started;
	internal->output = output_ptr;
	internal->context = (Ygg_Context) {
		.kind = Ygg_Context_Kind_Fiber,
		.coordinator = coordinator,
		.fiber_handle = handle
	};
	internal->registered_counter_count = 0;
	internal->owner_thread = NULL;
	
	// Retained by coordinator until fiber has been executed
	internal->rc = 1;
	
	// TODO: Pool these or something, don't use free/malloc
	internal->stack = malloc(YGG_FIBER_STACK_SIZE);
	
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
			
			Ygg_Fiber_Internal* internal = coordinator->fibers + context->fiber_handle.index;
			if (internal->generation != context->fiber_handle.generation) {
				return;
			}
			
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
			
			Ygg_Fiber_Internal* internal = coordinator->fibers + context->fiber_handle.index;
			if (internal->generation != context->fiber_handle.generation) {
				return;
			}
						
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
	ygg_spinlock_lock(&coordinator->counter_freelist_spinlock);
	ygg_assert(coordinator->counter_freelist_length > 0, "Counter limit exceeded");
	unsigned int index = coordinator->counter_freelist[--coordinator->counter_freelist_length];
	ygg_spinlock_unlock(&coordinator->counter_freelist_spinlock);
	
	Ygg_Counter_Internal* internal = coordinator->counters + index;
	Ygg_Counter_Handle handle = (Ygg_Counter_Handle) {
		.coordinator = coordinator,
		.index = index,
		.generation = ++internal->generation,
	};
		
	ygg_spinlock_init(&internal->spinlock);
	internal->rc = 1;
	internal->waiting = NULL;
	internal->counter = 0;
	internal->coordinator = coordinator;
	
	return handle;
}
void ygg_counter_retain(Ygg_Counter_Handle counter) {
	Ygg_Counter_Internal* internal = counter.coordinator->counters + counter.index;
	ygg_assert(internal->generation == counter.generation, "Invalid counter handle");
	atomic_fetch_add_explicit(&internal->rc, 1, memory_order_acq_rel);
}
void ygg_counter_release(Ygg_Counter_Handle counter) {
	Ygg_Counter_Internal* internal = counter.coordinator->counters + counter.index;
	ygg_assert(internal->generation == counter.generation, "Invalid counter handle");
	unsigned int prev = atomic_fetch_sub_explicit(&internal->rc, 1, memory_order_acq_rel);
	
	ygg_assert(prev > 0, "Don't underflow");
	if (prev == 1) {
		ygg_spinlock_lock(&counter.coordinator->counter_freelist_spinlock);
		counter.coordinator->counter_freelist[counter.coordinator->counter_freelist_length++] = counter.index;
		++internal->generation;
		ygg_spinlock_unlock(&counter.coordinator->counter_freelist_spinlock);
	}
}

void ygg_counter_increment(Ygg_Counter_Handle counter, unsigned int n) {
	Ygg_Counter_Internal* internal = counter.coordinator->counters + counter.index;
	ygg_assert(internal->generation == counter.generation, "Invalid counter handle");
	
	ygg_spinlock_lock(&internal->spinlock);
	atomic_fetch_add_explicit(&internal->counter, n, memory_order_acq_rel);
	ygg_spinlock_unlock(&internal->spinlock);
}
void ygg_counter_decrement(Ygg_Counter_Handle counter, unsigned int n) {
	Ygg_Counter_Internal* internal = counter.coordinator->counters + counter.index;
	ygg_assert(internal->generation == counter.generation, "Invalid counter handle");
	
	ygg_spinlock_lock(&internal->spinlock);
	unsigned int prev = atomic_fetch_sub_explicit(&internal->counter, n, memory_order_acq_rel);
	ygg_assert(prev > 0, "Don't underflow");
	if (prev == 1) {
		// Wake up blocked fibers
		Ygg_Context_Node* entry = internal->waiting;
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
	ygg_spinlock_unlock(&internal->spinlock);
}
void ygg_counter_await_completion(Ygg_Counter_Handle counter, Ygg_Fiber_Handle fiber_handle) {
	Ygg_Coordinator* coordinator = counter.coordinator;
	
	Ygg_Fiber_Internal* internal = coordinator->fibers + fiber_handle.index;
	if (internal->generation != fiber_handle.generation) {
		return;
	}
	
	ygg_spinlock_lock(&internal->spinlock);
	if (internal->state == Ygg_Fiber_Internal_State_Complete) {
		ygg_spinlock_unlock(&internal->spinlock);
	} else {
		ygg_counter_increment(counter, 1);
		internal->registered_counters[internal->registered_counter_count++] = counter;
		ygg_counter_retain(counter); // retained by fiber
		ygg_spinlock_unlock(&internal->spinlock);
	}
}

void ygg_counter_wait(Ygg_Counter_Handle counter, Ygg_Context* context) {
	Ygg_Coordinator* coordinator = counter.coordinator;

	Ygg_Counter_Internal* internal = counter.coordinator->counters + counter.index;
	ygg_assert(internal->generation == counter.generation, "Invalid counter handle");
	
	ygg_spinlock_lock(&internal->spinlock);
	if (internal->counter == 0) {
		ygg_spinlock_unlock(&internal->spinlock);
		return;
	}
	
	ygg_spinlock_lock(&coordinator->context_node_pool_spinlock);
	Ygg_Context_Node* entry = ygg_context_node_pool_acquire(&coordinator->context_node_pool);
	ygg_spinlock_unlock(&coordinator->context_node_pool_spinlock);
	
	*entry = (Ygg_Context_Node) {
		.context = context,
	};
	if (internal->waiting == NULL) {
		internal->waiting = entry;
	} else {
		entry->next = internal->waiting;
		internal->waiting = entry;
	}
	
	ygg_spinlock_unlock(&internal->spinlock);
	ygg_context_suspend(context);
}
