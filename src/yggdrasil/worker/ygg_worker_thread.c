
typedef struct Ygg_Worker_Thread {
	unsigned int thread_index;
	pthread_t thread;
	Ygg_Coordinator* coordinator;
		
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
	ygg_fiber_queue_init(&worker_thread->delayed_queue, YGG_QUEUE_SIZE);
	return worker_thread;
}
void ygg_worker_thread_destroy(Ygg_Worker_Thread* thread) {
	free(thread);
	*thread = (Ygg_Worker_Thread){};
}

void* _ygg_thread(void* data);
void ygg_worker_thread_start(Ygg_Worker_Thread* thread) {
	pthread_create(&thread->thread, NULL, _ygg_thread, thread);
}

void ygg_worker_thread_shutdown(Ygg_Worker_Thread* thread) {
	
}

ygg_internal bool ygg_worker_thread_next_fiber(Ygg_Worker_Thread* thread, Ygg_Fiber_Handle* handle) {
	// 1: Attempt to pop a fiber from the delayed queue
	if (ygg_fiber_queue_pop(&thread->delayed_queue, handle)) {
		ygg_coordinator_deref_fiber_handle(thread->coordinator, *handle);
		return true;
	}
	
	// 2: Attempt to pop an unstarted fiber from the coordinator
	if (ygg_coordinator_pop_fiber(thread->coordinator, handle)) {
		ygg_coordinator_deref_fiber_handle(thread->coordinator, *handle);
		return true;
	}
	
	return false;
}

void* _ygg_thread(void* data) {
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
		ygg_semaphore_lock(&coordinator->semaphore);
		while(!ygg_worker_thread_next_fiber(thread, &fiber_handle)) {
			// Wait for coordinator semaphore to update and tell us something has changed
			ygg_semaphore_wait(&coordinator->semaphore);
		}
		ygg_semaphore_unlock(&coordinator->semaphore);

		Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(coordinator, fiber_handle);
		
		ygg_update_thread_label("Fiber (index: %d) '%s'", fiber_handle.index, fiber_internal->fiber.label);
		if (fiber_internal->state == Ygg_Fiber_Internal_State_Not_Started) {
			printf("Thread %d: Starting fiber '%s'...\n", thread->thread_index, fiber_internal->fiber.label);
			
			fiber_internal->owner_thread = thread;
			
			ygg_spinlock_lock(&fiber_internal->spinlock);
			fiber_internal->state = Ygg_Fiber_Internal_State_Running;
			ygg_spinlock_unlock(&fiber_internal->spinlock);
						
			// NOTE: Not sure if == 0 is correct for fibers that resume more than once...
			ygg_cpu_state_store(fiber_internal->suspend_state);
			if (fiber_internal->state == Ygg_Fiber_Internal_State_Running) {
				void* sp = fiber_internal->stack + YGG_FIBER_STACK_SIZE;
				void* ctx = &fiber_internal->context;
				ygg_fiber_boot(sp, fiber_internal->fiber.func, ctx);
								
				printf("Thread %d: Completed fiber '%s'.\n", thread->thread_index, fiber_internal->fiber.label);
				ygg_spinlock_lock(&fiber_internal->spinlock);
				fiber_internal->state = Ygg_Fiber_Internal_State_Complete;
				ygg_spinlock_unlock(&fiber_internal->spinlock);
				ygg_future_fulfill(fiber_internal->future);
				ygg_future_release(fiber_internal->future);
				ygg_coordinator_fiber_release(coordinator, fiber_handle);
			}
		} else {
			printf("Thread %d: Resuming fiber '%s'...\n", thread->thread_index, fiber_internal->fiber.label);
			ygg_spinlock_lock(&fiber_internal->spinlock);
			fiber_internal->state = Ygg_Fiber_Internal_State_Running;
			ygg_spinlock_unlock(&fiber_internal->spinlock);
			ygg_cpu_state_restore(fiber_internal->resume_state);
		}
		printf("Thread %d: Left fiber\n", thread->thread_index);
		fiber_handle = (Ygg_Fiber_Handle) { };
		fiber_internal = NULL;
		ygg_update_thread_label("Idle");
	}
	
	return NULL;
}

void ygg_worker_thread_push_delayed_fiber(Ygg_Worker_Thread* thread, Ygg_Fiber_Handle handle) {
	Ygg_Fiber_Internal* fiber_internal = ygg_coordinator_deref_fiber_handle(thread->coordinator, handle);
	ygg_assert(fiber_internal->owner_thread == thread, "Fiber can only be pushed to execute on its owning thread");
	ygg_assert(fiber_internal->state == Ygg_Fiber_Internal_State_Suspended, "Fiber should be suspended");
	ygg_assert(fiber_internal->counter == 0, "Counter should be zero if the fiber is ready to resume");
	ygg_fiber_queue_push(&thread->delayed_queue, handle);
}
