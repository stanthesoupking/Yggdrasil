
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#define ygg_assert(cond, ...) \
if(!(cond)) { \
	printf("------------------------------------------------------------\n"); \
	printf("  Assertion failed on line %d of %s:  \n\t", __LINE__, __FILENAME__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
	printf("------------------------------------------------------------\n"); \
	abort();\
}

#define ygg_abort(...) \
printf("------------------------------------------------------------\n"); \
printf("  Abort on line %d of %s:  \n\t", __LINE__, __FILENAME__); \
printf(__VA_ARGS__); \
printf("\n"); \
printf("------------------------------------------------------------\n"); \
abort();

#define CONCAT(x, y) CONCAT2(x, y)
#define CONCAT2(x, y) x ## y

#define ygg_pool(element_type, pool_type, function_name) \
typedef struct pool_type { \
	unsigned int capacity; \
	element_type* backing; \
	unsigned int* free_list; \
	unsigned int free_list_length; \
} pool_type; \
__unused static void CONCAT(function_name, _init)(pool_type *pool, unsigned int capacity) { \
	*pool = (pool_type) { \
		.capacity = capacity, \
		.free_list_length = capacity, \
	}; \
	pool->backing = calloc(sizeof(element_type) * capacity, 1); \
	pool->free_list = calloc(sizeof(unsigned int) * capacity, 1); \
	for (unsigned int i = 0; i < capacity; i++) { \
		pool->free_list[i] = capacity - i - 1; \
	} \
} \
__unused static void CONCAT(function_name, _deinit)(pool_type *pool) { \
	free(pool->backing);\
	free(pool->free_list);\
} \
__unused static element_type* CONCAT(function_name, _acquire)(pool_type *pool) { \
	ygg_assert(pool->free_list_length > 0, "Pool exhausted."); \
	return pool->backing + pool->free_list[--pool->free_list_length]; \
} \
__unused static void CONCAT(function_name, _release)(pool_type *pool, element_type* item) { \
	ygg_assert((item >= pool->backing) && ((unsigned long long)(item - pool->backing) < pool->capacity), "Item does not belong in pool."); \
	unsigned int backing_index = ((unsigned int)(item - pool->backing)); \
	pool->free_list[pool->free_list_length++] = backing_index; \
	memset(pool->backing + backing_index, 0, sizeof(element_type)); \
} \
