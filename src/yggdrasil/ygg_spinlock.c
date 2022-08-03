
typedef struct Ygg_Spinlock {
	atomic_uchar locked;
} Ygg_Spinlock;

ygg_inline void ygg_spinlock_lock(Ygg_Spinlock* spinlock) {
	unsigned char expected = 0;
	for (;;) {
		if (atomic_compare_exchange_weak(&spinlock->locked, &expected, 1)) {
			return;
		}
	}
}

ygg_inline void ygg_spinlock_unlock(Ygg_Spinlock* spinlock) {
	spinlock->locked = 0;
}
