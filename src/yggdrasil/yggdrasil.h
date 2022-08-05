
// MARK: Macros

#define ygg_inline static inline
#define ygg_internal static inline
#define ygg_force_inline static inline __attribute__((always_inline))

// MARK: Fiber

typedef struct Ygg_Coordinator Ygg_Coordinator;

typedef struct Ygg_Fiber_Handle {
	unsigned int index;
	unsigned int generation;
} Ygg_Fiber_Handle;

typedef struct Ygg_Fiber_Ctx Ygg_Fiber_Ctx;
typedef void (*Ygg_Fiber_Func)(Ygg_Fiber_Ctx*);

// MARK: Coordinator

typedef struct Ygg_Coordinator Ygg_Coordinator;

typedef struct Ygg_Coordinator_Parameters {
	unsigned int thread_count;
} Ygg_Coordinator_Parameters;

Ygg_Coordinator* ygg_coordinator_new(Ygg_Coordinator_Parameters parameters);
void ygg_coordinator_destroy(Ygg_Coordinator* coordinator);

typedef struct Ygg_Fiber {
	const char* label;
	Ygg_Fiber_Func func;
} Ygg_Fiber;

ygg_inline Ygg_Fiber ygg_fiber(const char* label, Ygg_Fiber_Func func) {
	return (Ygg_Fiber) {
		.label = label,
		.func = func,
	};
}

typedef struct Ygg_Lazy_Result Ygg_Lazy_Result;
Ygg_Lazy_Result* ygg_lazy_result_retain(Ygg_Lazy_Result* result);
void ygg_lazy_result_release(Ygg_Lazy_Result* result);
void ygg_lazy_result_unwrap(Ygg_Fiber_Ctx* ctx, Ygg_Lazy_Result* result);

typedef enum Ygg_Priority {
	Ygg_Priority_Low = 0,
	Ygg_Priority_Normal = 1,
	Ygg_Priority_High = 2,
} Ygg_Priority;
#define YGG_PRIORITY_COUNT 3

Ygg_Lazy_Result* ygg_coordinator_dispatch(Ygg_Coordinator* coordinator, Ygg_Fiber fiber, Ygg_Priority priority);

// Current fiber functions
void ygg_fiber_increment_counter(Ygg_Fiber_Ctx* ctx, unsigned int n);
void ygg_fiber_wait_for_counter(Ygg_Fiber_Ctx* ctx);
Ygg_Coordinator* ygg_fiber_coordinator(Ygg_Fiber_Ctx* ctx);
