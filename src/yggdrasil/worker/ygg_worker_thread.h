
typedef struct Ygg_Worker_Thread Ygg_Worker_Thread;

Ygg_Worker_Thread* ygg_worker_thread_new(Ygg_Coordinator* coordinator, unsigned int thread_index);
void ygg_worker_thread_destroy(Ygg_Worker_Thread* thread);

void ygg_worker_thread_start(Ygg_Worker_Thread* thread);
void ygg_worker_thread_shutdown(Ygg_Worker_Thread* thread);

void ygg_worker_thread_push_delayed_fiber(Ygg_Worker_Thread* thread, Ygg_Fiber_Handle handle);
