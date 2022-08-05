
typedef struct Ygg_Spinlock {
	atomic_flag locked;
} Ygg_Spinlock;

ygg_inline void ygg_spinlock_lock(Ygg_Spinlock* spinlock) {
	while (atomic_flag_test_and_set_explicit(&spinlock->locked, memory_order_acquire)) {}
}

ygg_inline void ygg_spinlock_unlock(Ygg_Spinlock* spinlock) {
	atomic_flag_clear(&spinlock->locked);
}
