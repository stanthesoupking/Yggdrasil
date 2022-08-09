/**
 * Yggdrasil - v1.0 - userland fiber libary.
 * No warranty; use at your own risk.
 *
 * For usage, see the 'examples' folder.
 *
 * Supported compilers: clang, gnu-c
 * Supported architectures: arm64
 */

// ----------------------------------
//           Coordinator
// ----------------------------------
typedef struct Ygg_Coordinator_Parameters {
	unsigned int thread_count;
} Ygg_Coordinator_Parameters;

typedef struct Ygg_Coordinator Ygg_Coordinator;
Ygg_Coordinator* ygg_coordinator_new(Ygg_Coordinator_Parameters parameters);
void ygg_coordinator_destroy(Ygg_Coordinator* coordinator);

// ----------------------------------
//             Context
// ----------------------------------
typedef struct Ygg_Context Ygg_Context;
Ygg_Context* ygg_blocking_context_new(Ygg_Coordinator* coordinator);
void ygg_blocking_context_destroy(Ygg_Context* blocking_context);
Ygg_Coordinator* ygg_context_coordinator(Ygg_Context* context);

// ----------------------------------
//              Fibers
// ----------------------------------
// As an alternative to calling the generic dispatch functions, you can
// use the following set of macros to declare a fibers and their input/
// output parameters ahead of dispatch. This allows for type-safety to
// be enforced and is the recommended method of dispatching fibers.
//
// To declare a fiber, call the applicable macro:
//   - ygg_fiber_declare(name, function):
//       fiber has no input or output
//
//   - ygg_fiber_declare_in(name, function, in_type):
//       fiber takes an input parameter but has no output
//
//   - ygg_fiber_declare_out(name, function, out_type):
//       fiber outputs a value but has no input
//
//   - ygg_fiber_declare_inout(name, function, in_type, out_type):
//       fiber takes an input parameter and outputs a value

typedef struct Ygg_Fiber_Handle {
	unsigned int index;
	unsigned int generation;
} Ygg_Fiber_Handle;

typedef enum Ygg_Priority {
	Ygg_Priority_Low = 0,
	Ygg_Priority_Normal = 1,
	Ygg_Priority_High = 2,
} Ygg_Priority;
#define YGG_PRIORITY_COUNT 3

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

// ----------------------------------
//             Counter
// ----------------------------------
// Synchronisation primitive for handling asynchronous completion of fibers
// and threads from outside of Yggdrasil.
typedef struct Ygg_Counter_Handle {
	Ygg_Coordinator* coordinator;
	unsigned int index;
	unsigned int generation;
} Ygg_Counter_Handle;

Ygg_Counter_Handle ygg_counter_new(Ygg_Coordinator* coordinator);
void ygg_counter_retain(Ygg_Counter_Handle counter);
void ygg_counter_release(Ygg_Counter_Handle counter);

void ygg_counter_increment(Ygg_Counter_Handle counter, unsigned int n);
void ygg_counter_decrement(Ygg_Counter_Handle counter, unsigned int n);

// Increment counter by 1, and decrement counter by 1 when fiber completes
void ygg_counter_await_completion(Ygg_Counter_Handle counter, Ygg_Fiber_Handle fiber_handle);

// Blocks the current fiber/thread until the counter hits 0
void ygg_counter_wait(Ygg_Counter_Handle counter, Ygg_Context* context);

// ----------------------------------
//      Generic Fiber Dispatch
// ----------------------------------
// These functions are intended to be used by the `ygg_fiber_declare` set of
// macros, which enforce type safety around the `void*` parameters.
// Although I can't stop you from using these if you prefer.
Ygg_Fiber_Handle ygg_dispatch_generic_async(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output);
void ygg_dispatch_generic_sync(Ygg_Context* context, Ygg_Fiber fiber, Ygg_Priority priority, void* input, unsigned int input_length, void* output);

// ~~~ Macro hell beyond this point, venture forth at your own risk... ~~~
#define ygg_disable_asan __attribute__((no_sanitize("address")))

#define YGG_CONCAT(x, y) YGG_CONCAT2(x, y)
#define YGG_CONCAT2(x, y) x ## y
#define YGG_STRING(s) #s
#define ygg_fiber_declare(name, function) \
	ygg_disable_asan __unused static inline void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		void (*func_ptr)(Ygg_Context*) = function;\
		func_ptr(context);\
	}\
	__unused static inline Ygg_Fiber_Handle YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic_async(context, fiber, priority, NULL, 0, NULL);\
	}\
	__unused static inline void YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, NULL, 0, NULL);\
	}\

#define ygg_fiber_declare_in(name, function, in_type) \
	ygg_disable_asan __unused static inline void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		in_type in = *((in_type*)in_ptr);\
		void (*func_ptr)(Ygg_Context*,in_type) = function;\
		func_ptr(context, in);\
	}\
	__unused static inline Ygg_Fiber_Handle YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority, in_type in) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic_async(context, fiber, priority, &in, sizeof(in_type), NULL);\
	}\
	__unused static inline void YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority, in_type in) 	{\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, &in, 	sizeof(in_type), NULL);\
	}\

#define ygg_fiber_declare_out(name, function, out_type) \
	ygg_disable_asan __unused static inline void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		out_type (*func_ptr)(Ygg_Context*) = function;\
		*((out_type*)out_ptr) = func_ptr(context);\
	}\
	__unused static inline Ygg_Fiber_Handle YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority, out_type* out) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic_async(context, fiber, priority, NULL, 0, out);\
	}\
	__unused static inline out_type YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority) 	{\
		out_type out;\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, NULL, 0, &out);\
		return out;\
	}\

#define ygg_fiber_declare_inout(name, function, in_type, out_type) \
	ygg_disable_asan __unused static inline void YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap))(Ygg_Context* context, void* in_ptr, void* out_ptr) { \
		in_type in = *((in_type*)in_ptr);\
		out_type (*func_ptr)(Ygg_Context*,in_type) = function;\
		*((out_type*)out_ptr) = func_ptr(context, in);\
	}\
	__unused static inline Ygg_Fiber_Handle YGG_CONCAT(name, _dispatch_async)(Ygg_Context* context, Ygg_Priority priority, in_type in, out_type* out) {\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		return ygg_dispatch_generic_async(context, fiber, priority, &in, sizeof(in_type), out);\
	}\
	__unused static inline out_type YGG_CONCAT(name, _dispatch_sync)(Ygg_Context* context, Ygg_Priority priority, in_type in) 	{\
		out_type out;\
		Ygg_Fiber fiber = (Ygg_Fiber){ YGG_STRING(name), YGG_CONCAT(_, YGG_CONCAT(name, _bootstrap)) };\
		ygg_dispatch_generic_sync(context, fiber, priority, &in, 	sizeof(in_type), &out);\
		return out;\
	}\
