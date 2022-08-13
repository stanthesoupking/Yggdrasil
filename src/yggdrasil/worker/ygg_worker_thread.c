
typedef struct Ygg_Worker_Thread {
	unsigned int thread_index;
	pthread_t thread;
	Ygg_Coordinator* coordinator;
	Ygg_Semaphore semaphore;
	
	Ygg_Fiber_Queue delayed_queue;
} Ygg_Worker_Thread;

#define ygg_update_thread_label(...) \
	snprintf(thread_label_postfix, sizeof(thread_label_postfix), __VA_ARGS__); \
	snprintf(thread_label_full, sizeof(thread_label_full), "%s [%s]", thread_label_prefix, thread_label_postfix); \
	pthread_setname_np(thread_label_full); \

Ygg_Worker_Thread* ygg_worker_thread_new(Ygg_Coordinator* coordinator, unsigned int thread_index) {
	Ygg_Worker_Thread* worker_thread = malloc(sizeof(Ygg_Worker_Thread));
	*worker_thread = (Ygg_Worker_Thread) {
		.coordinator = coordinator,
		.thread_index = thread_index,
	};
	ygg_semaphore_init(&worker_thread->semaphore);
	ygg_fiber_queue_init(&worker_thread->delayed_queue, 64);
	return worker_thread;
}
void ygg_worker_thread_destroy(Ygg_Worker_Thread* thread) {
	ygg_semaphore_deinit(&thread->semaphore);
	ygg_fiber_queue_deinit(&thread->delayed_queue);
	free(thread);
}
void* _ygg_thread(void* data);
void ygg_worker_thread_start(Ygg_Worker_Thread* thread) {
	pthread_create(&thread->thread, NULL, _ygg_thread, thread);
}
void ygg_worker_thread_join(Ygg_Worker_Thread* thread) {
	pthread_join(thread->thread, NULL);
}

ygg_internal bool ygg_worker_thread_next_fiber(Ygg_Worker_Thread* thread, Ygg_Fiber_Handle* handle) {
	// 1: Attempt to pop a fiber from the delayed queue
	if (ygg_fiber_queue_pop(&thread->delayed_queue, handle)) {
		return true;
	}
	
	// 2: Attempt to pop an unstarted fiber from the coordinator
	if (ygg_coordinator_pop_fiber(thread->coordinator, handle)) {
		return true;
	}
	
	return false;
}

// NOTE: Disabling optimisations to prevent inline assembly from being shifted around
// NOTE: Disabling ASan to avoid false-positives due to altering the stack pointer
void* ygg_disable_optimisations ygg_disable_asan _ygg_thread(void* data) {
	Ygg_Worker_Thread* thread = data;
	Ygg_Coordinator* coordinator = thread->coordinator;
	
	// Thread label for debugging:
	char thread_label_prefix[32];
	char thread_label_postfix[128];
	char thread_label_full[160];
	snprintf(thread_label_prefix, sizeof(thread_label_prefix), "Yggdrasil %d", thread->thread_index);
		
	ygg_update_thread_label("Idle");
	bool alive = true;
	while (alive) {
		// Get next fiber
		Ygg_Fiber_Handle fiber_handle;
		while(!ygg_worker_thread_next_fiber(thread, &fiber_handle)) {
			if (coordinator->shutting_down && (thread->delayed_queue.count == 0) && !ygg_coordinator_has_nonempty_queue(coordinator)) {
				return NULL;
			}
			
			// Wait for coordinator semaphore to update and tell us something has changed
			ygg_semaphore_wait(&thread->semaphore);
		}

		Ygg_Fiber_Internal* fiber_internal = coordinator->fibers + fiber_handle.index;
		ygg_assert(fiber_internal->generation == fiber_handle.generation, "Invalid fiber handle");
		
		ygg_update_thread_label("Fiber '%s'", fiber_internal->fiber.label);
		if (fiber_internal->state == Ygg_Fiber_Internal_State_Not_Started) {
			// printf("Thread %d: Starting fiber '%s' (idx: %d, gen: %d)...\n", thread->thread_index, fiber_internal->fiber.label, fiber_handle.index, fiber_handle.generation);
			
			fiber_internal->owner_thread = thread;
			
			ygg_spinlock_lock(&fiber_internal->spinlock);
			fiber_internal->state = Ygg_Fiber_Internal_State_Running;
			ygg_spinlock_unlock(&fiber_internal->spinlock);
						
			ygg_cpu_state_store(fiber_internal->suspend_state);
			if (fiber_internal->state == Ygg_Fiber_Internal_State_Running) {
				void* sp = fiber_internal->stack + YGG_FIBER_STACK_SIZE;
				void* ctx = &fiber_internal->context;
				void* input = fiber_internal->input;
				void* output = fiber_internal->output;
				ygg_fiber_boot(sp, fiber_internal->fiber.func, ctx, input, output);
								
				// printf("Thread %d: Completed fiber '%s' (idx: %d, gen: %d).\n", thread->thread_index, fiber_internal->fiber.label, fiber_handle.index, fiber_handle.generation);
				ygg_spinlock_lock(&fiber_internal->spinlock);
				fiber_internal->state = Ygg_Fiber_Internal_State_Complete;
				
				ygg_assert(fiber_internal != NULL, "Fiber internal can't be NULL");
				
				// Decrement and release all registered counters
				ygg_spinlock_lock(&coordinator->counter_node_pool_spinlock);
				Ygg_Counter_Node* counter_node = fiber_internal->registered_counters;
				while (counter_node != NULL) {
					ygg_counter_decrement(counter_node->handle, 1);
					ygg_counter_release(counter_node->handle);
					Ygg_Counter_Node* next = counter_node->next;
					ygg_counter_node_pool_release(&coordinator->counter_node_pool, counter_node);
					counter_node = next;
				}
				ygg_spinlock_unlock(&coordinator->counter_node_pool_spinlock);
				ygg_coordinator_fiber_release(coordinator, fiber_handle);
				ygg_spinlock_unlock(&fiber_internal->spinlock);
			}
		} else {
			// printf("Thread %d: Resuming fiber '%s' (idx: %d, gen: %d)...\n", thread->thread_index, fiber_internal->fiber.label, fiber_handle.index, fiber_handle.generation);
			ygg_spinlock_lock(&fiber_internal->spinlock);
			fiber_internal->state = Ygg_Fiber_Internal_State_Running;
			ygg_spinlock_unlock(&fiber_internal->spinlock);
			ygg_cpu_state_restore(fiber_internal->resume_state);
		}
		// printf("Thread %d: Left fiber (idx: %d, gen: %d)\n", thread->thread_index, fiber_handle.index, fiber_handle.generation);
		fiber_handle = (Ygg_Fiber_Handle) { };
		fiber_internal = NULL;
		ygg_update_thread_label("Idle");
	}
	
	return NULL;
}

void ygg_worker_thread_push_delayed_fiber(Ygg_Worker_Thread* thread, Ygg_Fiber_Handle fiber_handle) {
	Ygg_Coordinator* coordinator = thread->coordinator;
	Ygg_Fiber_Internal* fiber_internal = coordinator->fibers + fiber_handle.index;
	ygg_assert(fiber_internal->generation == fiber_handle.generation, "Invalid fiber handle");
	ygg_assert(fiber_internal->owner_thread == thread, "Fiber can only be pushed to execute on its owning thread");
	ygg_assert(fiber_internal->state == Ygg_Fiber_Internal_State_Suspended, "Fiber should be suspended");
	ygg_fiber_queue_push(&thread->delayed_queue, fiber_handle);
	ygg_semaphore_signal(&thread->semaphore);
}

Ygg_Semaphore* ygg_worker_thread_semaphore(Ygg_Worker_Thread* thread) {
	return &thread->semaphore;
}
