/**
 * Yggdrasil - v1.0 - userland fiber libary.
 * No warranty; use at your own risk.
 *
 * For usage, see the 'examples' folder.
 *
 * Supported compilers: clang, gnu-c
 * Supported architectures: arm64
 */

// MARK: Fiber

typedef struct Ygg_Context Ygg_Context;
typedef void (*Ygg_Fiber_Func)(Ygg_Context* context, void* arguments);

typedef struct Ygg_Fiber {
	const char* label;
	Ygg_Fiber_Func func;
} Ygg_Fiber;
static inline Ygg_Fiber ygg_fiber(const char* label, Ygg_Fiber_Func func) {
	return (Ygg_Fiber) {
		.label = label,
		.func = func,
	};
}

// MARK: Coordinator

typedef struct Ygg_Coordinator Ygg_Coordinator;

typedef struct Ygg_Coordinator_Parameters {
	unsigned int thread_count;
} Ygg_Coordinator_Parameters;

Ygg_Coordinator* ygg_coordinator_new(Ygg_Coordinator_Parameters parameters);
void ygg_coordinator_destroy(Ygg_Coordinator* coordinator);

// MARK: Blocking Context

Ygg_Context* ygg_blocking_context_new(Ygg_Coordinator* coordinator);
void ygg_blocking_context_destroy(Ygg_Context* blocking_context);

// MARK: Future

typedef struct Ygg_Future Ygg_Future;

Ygg_Future* ygg_future_retain(Ygg_Future* future);
void ygg_future_release(Ygg_Future* future);

void ygg_store_result(Ygg_Context* ctx, void* data, unsigned int data_length);

// NOTE: Result is only valid while future is retained.
const void* ygg_unwrap(Ygg_Context* context, Ygg_Future* future);

void ygg_await(Ygg_Context* context, Ygg_Future* future);

// MARK: Counter

void ygg_increment_counter(Ygg_Context* ctx, unsigned int n);
void ygg_decrement_counter(Ygg_Context* ctx, unsigned int n);
void ygg_wait_for_counter(Ygg_Context* ctx);

// MARK: Dispatch

typedef enum Ygg_Priority {
	Ygg_Priority_Low = 0,
	Ygg_Priority_Normal = 1,
	Ygg_Priority_High = 2,
} Ygg_Priority;
#define YGG_PRIORITY_COUNT 3

Ygg_Future* ygg_dispatch(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* args, unsigned int args_length);
Ygg_Future* ygg_dispatch_sync(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* args, unsigned int args_length);
