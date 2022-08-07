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
typedef void (*Ygg_Fiber_Func)(Ygg_Context* context, void* input, void* output);

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

Ygg_Future* ygg_dispatch_generic(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output);
void ygg_dispatch_generic_sync(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output);

// Fiber declaration macros:
//  - ygg_fiber_declare(name, function)
//  - ygg_fiber_declare_in(name, function, in_type)
//  - ygg_fiber_declare_out(name, function, out_type)
//  - ygg_fiber_declare_inout(name, function, in_type, out_type)

#define YGG_CONCAT(x, y) YGG_CONCAT2(x, y)
#define YGG_CONCAT2(x, y) x ## y
#define YGG_STRING(s) #s

#define ygg_fiber_declare(name, function) \
	void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		void (*func_ptr)(Ygg_Context*) = function;\
		func_ptr(context);\
	}\
	Ygg_Future* YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic(context, fiber, priority, NULL, 0, NULL);\
	}\
	void YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, NULL, 0, NULL);\
	}\

#define ygg_fiber_declare_in(name, in_type, function) \
	void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		in_type in = *((in_type*)in_ptr);\
		void (*func_ptr)(Ygg_Context*,in_type) = function;\
		func_ptr(context, in);\
	}\
	Ygg_Future* YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority, in_type in) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic(context, fiber, priority, &in, sizeof(in_type), NULL);\
	}\
	void YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority, in_type in) 	{\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, &in, 	sizeof(in_type), NULL);\
	}\

#define ygg_fiber_declare_out(name, out_type, function) \
	void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		out_type (*func_ptr)(Ygg_Context*) = function;\
		*((out_type*)out_ptr) = func_ptr(context);\
	}\
	Ygg_Future* YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority, out_type* out) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic(context, fiber, priority, NULL, 0, out);\
	}\
	out_type YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority) 	{\
		out_type out;\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, NULL, 0, &out);\
		return out;\
	}\

#define ygg_fiber_declare_inout(name, in_type, out_type, function) \
	void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		in_type in = *((in_type*)in_ptr);\
		out_type (*func_ptr)(Ygg_Context*,in_type) = function;\
		*((out_type*)out_ptr) = func_ptr(context, in);\
	}\
	Ygg_Future* YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority, in_type in, out_type* out) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic(context, fiber, priority, &in, sizeof(in_type), out);\
	}\
	out_type YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority, in_type in) 	{\
		out_type out;\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, &in, 	sizeof(in_type), &out);\
		return out;\
	}\
