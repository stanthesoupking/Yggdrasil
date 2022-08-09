
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
