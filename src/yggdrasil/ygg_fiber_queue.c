
typedef struct Ygg_Fiber_Queue {
	Ygg_Fiber_Handle* handles;
	atomic_uint head, tail;
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
	for (;;) {
		unsigned int tail = queue->tail;
		unsigned int new_tail = (tail + 1) % queue->capacity;
		if (atomic_compare_exchange_strong(&queue->tail, &tail, new_tail)) {
			ygg_assert(queue->count + 1 < queue->capacity, "Queue capacity exceeded");
			++queue->count;
			queue->handles[tail] = handle;
			return;
		}
	}
}
ygg_internal bool ygg_fiber_queue_pop(Ygg_Fiber_Queue* queue, Ygg_Fiber_Handle* handle) {
	while (queue->head != queue->tail) {
		unsigned int head = queue->head;
		unsigned int new_head = (head + 1) % queue->capacity;
		if (atomic_compare_exchange_strong(&queue->head, &head, new_head)) {
			--queue->count;
			*handle = queue->handles[head];
			return true;
		}
	}
	return false;
}
