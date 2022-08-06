
typedef struct Ygg_Mutex {
	pthread_mutex_t mutex;
} Ygg_Mutex;

void ygg_mutex_init(Ygg_Mutex* mutex) {
	pthread_mutex_init(&mutex->mutex, NULL);
}

void ygg_mutex_deinit(Ygg_Mutex* mutex) {
	pthread_mutex_destroy(&mutex->mutex);
}

void ygg_mutex_lock(Ygg_Mutex* mutex) {
	pthread_mutex_lock(&mutex->mutex);
}

void ygg_mutex_unlock(Ygg_Mutex* mutex) {
	pthread_mutex_unlock(&mutex->mutex);
}

typedef struct Ygg_Semaphore {
	pthread_cond_t cond;
	pthread_mutex_t mutex;
} Ygg_Semaphore;

void ygg_semaphore_init(Ygg_Semaphore* semaphore) {
	pthread_cond_init(&semaphore->cond, NULL);
	pthread_mutex_init(&semaphore->mutex, NULL);
}

ygg_inline void ygg_semaphore_signal(Ygg_Semaphore* semaphore) {
	pthread_cond_signal(&semaphore->cond);
}
ygg_inline void ygg_semaphore_broadcast(Ygg_Semaphore* semaphore) {
	pthread_cond_broadcast(&semaphore->cond);
}
ygg_inline void ygg_semaphore_wait(Ygg_Semaphore* semaphore) {
	pthread_cond_wait(&semaphore->cond, &semaphore->mutex);
}

ygg_inline void ygg_semaphore_lock(Ygg_Semaphore* semaphore) {
	pthread_mutex_lock(&semaphore->mutex);
}
ygg_inline void ygg_semaphore_unlock(Ygg_Semaphore* semaphore) {
	pthread_mutex_unlock(&semaphore->mutex);
}
