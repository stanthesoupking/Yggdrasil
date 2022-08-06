
typedef struct Ygg_Fiber_Handle {
	unsigned int index;
	unsigned int generation;
} Ygg_Fiber_Handle;

void ygg_future_fulfill(Ygg_Future* future);
