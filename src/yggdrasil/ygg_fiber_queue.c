
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
