// Yggdrasil implementation

#include "yggdrasil.h"

#define YGG_FIBER_STACK_SIZE 128 * 1024 // 128KB
#define YGG_MAXIMUM_INPUT_LENGTH 64

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

// MARK: Macro

#define ygg_inline static inline
#define ygg_internal static inline
#define ygg_force_inline static inline __attribute__((always_inline))

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#define ygg_assert(cond, ...) \
if(!(cond)) { \
	printf("------------------------------------------------------------\n"); \
	printf("  Assertion failed on line %d of %s:  \n\t", __LINE__, __FILENAME__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
	printf("------------------------------------------------------------\n"); \
	abort();\
}

#define ygg_abort(...) \
printf("------------------------------------------------------------\n"); \
printf("  Abort on line %d of %s:  \n\t", __LINE__, __FILENAME__); \
printf(__VA_ARGS__); \
printf("\n"); \
printf("------------------------------------------------------------\n"); \
abort();

#define CONCAT(x, y) CONCAT2(x, y)
#define CONCAT2(x, y) x ## y

#define ygg_pool(element_type, pool_type, function_name) \
typedef struct pool_type { \
	unsigned int capacity; \
	element_type* backing; \
	unsigned int* free_list; \
	unsigned int free_list_length; \
} pool_type; \
__unused static void CONCAT(function_name, _init)(pool_type *pool, unsigned int capacity) { \
	*pool = (pool_type) { \
		.capacity = capacity, \
		.free_list_length = capacity, \
	}; \
	pool->backing = calloc(sizeof(element_type) * capacity, 1); \
	pool->free_list = calloc(sizeof(unsigned int) * capacity, 1); \
	for (unsigned int i = 0; i < capacity; i++) { \
		pool->free_list[i] = capacity - i - 1; \
	} \
} \
__unused static void CONCAT(function_name, _deinit)(pool_type *pool) { \
	free(pool->backing);\
	free(pool->free_list);\
} \
__unused static element_type* CONCAT(function_name, _acquire)(pool_type *pool) { \
	ygg_assert(pool->free_list_length > 0, "Pool exhausted."); \
	return pool->backing + pool->free_list[--pool->free_list_length]; \
} \
__unused static void CONCAT(function_name, _release)(pool_type *pool, element_type* item) { \
	ygg_assert((item >= pool->backing) && ((unsigned long long)(item - pool->backing) < pool->capacity), "Item does not belong in pool."); \
	unsigned int backing_index = ((unsigned int)(item - pool->backing)); \
	pool->free_list[pool->free_list_length++] = backing_index; \
	memset(pool->backing + backing_index, 0, sizeof(element_type)); \
} \

// MARK: CPU

#if defined(__GNUC__)
	#if defined(__aarch64__)
		#define ygg_cpu_clobber_all_registers() asm volatile ("":::"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7", "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15", "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23", "w24", "w25", "w26", "w27", "w28", "cc")

		typedef struct Ygg_CPU_State {
			void* reg[4];
		} Ygg_CPU_State;
		#define ygg_cpu_state_store(state) \
			ygg_cpu_clobber_all_registers();\
			asm volatile ("mov x0, sp\n"\
						  "str x0, %0\n" /* store sp */\
						  "mov x0, lr\n"\
						  "str x0, %1\n" /* store lr */\
						  "mov x0, fp\n"\
						  "str x0, %3\n" /* store frame pointer */\
						  "adr x0, #0\n"\
						  "str x0, %2\n" /* store pc */\
						  : "+m"(state.reg[0]), "+m"(state.reg[1]), "+m"(state.reg[2]), "+m"(state.reg[3])\
						  :\
						  : "x0", "memory"\
						  )\
		
		#define ygg_cpu_state_restore(state) \
			asm volatile ("ldr x0, %0\n"\
						  "mov sp, x0\n" /* restore sp */\
						  "ldr x0, %1\n"\
						  "mov lr, x0\n" /* restore lr */\
						  "ldr x0, %3\n"\
						  "mov fp, x0\n" /* restore frame pointer */\
						  "ldr x0, %2\n"\
						  "br x0\n" /* restore pc */\
						  :\
						  : "m"(state.reg[0]), "m"(state.reg[1]), "m"(state.reg[2]), "m"(state.reg[3])\
						  : "x0"\
						  )\
		
		#define ygg_fiber_boot(stack_ptr, func, context, input, output) \
			ygg_cpu_clobber_all_registers();\
			asm volatile(\
				/* Set sp and push current sp to the new stack for restoring when fiber ends. */\
				"mov x1, sp\n"\
				"mov sp, %0\n"\
				"sub sp, sp, #16\n"\
				"str x1, [sp]\n"\
				/* Call func */\
				"mov x0, %2\n"\
				"mov x1, %3\n"\
				"mov x2, %4\n"\
				"blr %1\n"\
				/* Restore original sp */\
				"ldr x1, [sp]\n"\
				"mov sp, x1\n"\
				:\
				: "r"(stack_ptr), "r"(func), "r"(context), "r"(input), "r"(output) \
				: "x0", "x1", "x2" \
			)
	#else
		#error "unsupported architecture"
	#endif
#else
	#error "unsupported compiler"
#endif

// MARK: Spinlock

typedef struct Ygg_Spinlock {
	atomic_flag locked;
} Ygg_Spinlock;
ygg_inline void ygg_spinlock_init(Ygg_Spinlock* spinlock) {
	spinlock->locked = (atomic_flag)ATOMIC_FLAG_INIT;
}
ygg_inline void ygg_spinlock_lock(Ygg_Spinlock* spinlock) {
	while (atomic_flag_test_and_set_explicit(&spinlock->locked, memory_order_acquire)) {}
}

ygg_inline void ygg_spinlock_unlock(Ygg_Spinlock* spinlock) {
	atomic_flag_clear(&spinlock->locked);
}

// MARK: Semaphore

typedef struct Ygg_Semaphore {
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	bool signalled;
} Ygg_Semaphore;
ygg_inline void ygg_semaphore_init(Ygg_Semaphore* semaphore) {
	pthread_cond_init(&semaphore->cond, NULL);
	pthread_mutex_init(&semaphore->mutex, NULL);
}
ygg_inline void ygg_semaphore_deinit(Ygg_Semaphore* semaphore) {
	pthread_cond_destroy(&semaphore->cond);
	pthread_mutex_destroy(&semaphore->mutex);
}
ygg_inline void ygg_semaphore_signal(Ygg_Semaphore* semaphore) {
	pthread_mutex_lock(&semaphore->mutex);
	semaphore->signalled = true;
	pthread_mutex_unlock(&semaphore->mutex);
	pthread_cond_signal(&semaphore->cond);
}
ygg_inline void ygg_semaphore_broadcast(Ygg_Semaphore* semaphore) {
	pthread_cond_broadcast(&semaphore->cond);
}
ygg_inline void ygg_semaphore_wait(Ygg_Semaphore* semaphore) {
	pthread_mutex_lock(&semaphore->mutex);
	if (!semaphore->signalled) {
		pthread_cond_wait(&semaphore->cond, &semaphore->mutex);
	}
	semaphore->signalled = false;
	pthread_mutex_unlock(&semaphore->mutex);
}

// MARK: Fiber Queue

typedef struct Ygg_Fiber_Queue {
	Ygg_Spinlock spinlock;
	Ygg_Fiber_Handle* handles;
	unsigned int head, tail;
	unsigned int count;
	unsigned int capacity;
} Ygg_Fiber_Queue;

ygg_internal void ygg_fiber_queue_init(Ygg_Fiber_Queue* queue, unsigned int capacity) {
	*queue = (Ygg_Fiber_Queue) {
		.handles = malloc(sizeof(Ygg_Fiber_Handle) * capacity),
		.capacity = capacity,
	};
}
ygg_internal void ygg_fiber_queue_deinit(Ygg_Fiber_Queue* queue) {
	free(queue->handles);
	*queue = (Ygg_Fiber_Queue){};
}

ygg_internal void ygg_fiber_queue_push(Ygg_Fiber_Queue* queue, Ygg_Fiber_Handle handle) {
	ygg_spinlock_lock(&queue->spinlock);
	
	ygg_assert(queue->count + 1 < queue->capacity, "Queue capacity exceeded");
	++queue->count;
	
	queue->handles[queue->tail] = handle;
	queue->tail = (queue->tail + 1) % queue->capacity;
	
	ygg_spinlock_unlock(&queue->spinlock);
}
ygg_internal bool ygg_fiber_queue_pop(Ygg_Fiber_Queue* queue, Ygg_Fiber_Handle* handle) {
	ygg_spinlock_lock(&queue->spinlock);
	if (queue->count > 0) {
		*handle = queue->handles[queue->head];
		--queue->count;
		queue->head = (queue->head + 1) % queue->capacity;
		ygg_spinlock_unlock(&queue->spinlock);
		return true;
	}
	ygg_spinlock_unlock(&queue->spinlock);
	return false;
}

// MARK: Worker Thread (Definition)

typedef struct Ygg_Worker_Thread Ygg_Worker_Thread;

Ygg_Worker_Thread* ygg_worker_thread_new(Ygg_Coordinator* coordinator, unsigned int thread_index);
void ygg_worker_thread_destroy(Ygg_Worker_Thread* thread);

void ygg_worker_thread_start(Ygg_Worker_Thread* thread);
void ygg_worker_thread_join(Ygg_Worker_Thread* thread);

void ygg_worker_thread_push_delayed_fiber(Ygg_Worker_Thread* thread, Ygg_Fiber_Handle fiber_handle);
Ygg_Semaphore* ygg_worker_thread_semaphore(Ygg_Worker_Thread* thread);

// MARK: Coordinator

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

typedef struct Ygg_Counter_Node Ygg_Counter_Node;
typedef struct Ygg_Counter_Node {
	Ygg_Counter_Handle handle;
	Ygg_Counter_Node* next;
} Ygg_Counter_Node;
ygg_pool(Ygg_Counter_Node, Ygg_Counter_Node_Pool, ygg_counter_node_pool);

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
	
	Ygg_Counter_Node* registered_counters;
	
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
	
	Ygg_Spinlock counter_node_pool_spinlock;
	Ygg_Counter_Node_Pool counter_node_pool;
	
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
		.fibers = calloc(1, sizeof(Ygg_Fiber_Internal) * parameters.maximum_fibers),
		.counter_freelist = malloc(sizeof(unsigned int) * parameters.maximum_counters),
		.counters = calloc(1, sizeof(Ygg_Counter_Internal) * parameters.maximum_counters),
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
	ygg_counter_node_pool_init(&coordinator->counter_node_pool, parameters.maximum_fibers);
		
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
	ygg_counter_node_pool_deinit(&coordinator->counter_node_pool);
	
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
	internal->registered_counters = NULL;
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

// NOTE: Disabling optimisations to prevent inline assembly from being shifted around
void ygg_disable_optimisations ygg_context_suspend(Ygg_Context* context) {
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
	ygg_spinlock_lock(&internal->spinlock);
	if (internal->generation != fiber_handle.generation) {
		ygg_spinlock_unlock(&internal->spinlock);
		return;
	}
	
	if (internal->state == Ygg_Fiber_Internal_State_Complete) {
		ygg_spinlock_unlock(&internal->spinlock);
	} else {
		ygg_counter_increment(counter, 1);
		
		
		ygg_spinlock_lock(&coordinator->counter_node_pool_spinlock);
		Ygg_Counter_Node* counter_node = ygg_counter_node_pool_acquire(&coordinator->counter_node_pool);
		ygg_spinlock_unlock(&coordinator->counter_node_pool_spinlock);
		
		*counter_node = (Ygg_Counter_Node) {
			.handle = counter,
			.next = internal->registered_counters,
		};
		internal->registered_counters = counter_node;
		
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

// MARK: Worker Thread (Implementation)

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
