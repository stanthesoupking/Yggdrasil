
#define ygg_inline static inline
#define ygg_internal static inline
#define ygg_force_inline static inline __attribute__((always_inline))

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

#define ygg_growable_pool(element_type, handle_type, pool_type, function_name, chunk_size) \
typedef struct pool_type { \
	element_type** chunks; \
	unsigned int chunk_count; \
	unsigned int* free_list; \
	unsigned int free_list_length; \
	unsigned int count; \
} pool_type; \
__unused static void CONCAT(function_name, _init)(pool_type *pool, unsigned int initial_chunk_count) { \
	*pool = (pool_type) { \
		.free_list_length = 0, \
		.chunk_count = initial_chunk_count, \
	}; \
	pool->chunks = malloc(sizeof(element_type*) * initial_chunk_count); \
	for (unsigned int i = 0; i < initial_chunk_count; ++i) { \
		pool->chunks[i] = calloc(sizeof(element_type) * chunk_size, 1); \
	} \
	pool->free_list = calloc(sizeof(unsigned int) * initial_chunk_count * chunk_size, 1); \
} \
__unused static void CONCAT(function_name, _deinit)(pool_type *pool) { \
	for (unsigned int i = 0; i < pool->chunk_count; ++i) {\
		free(pool->chunks[i]);\
	}\
	free(pool->chunks);\
	free(pool->free_list);\
} \
__unused static handle_type CONCAT(function_name, _acquire)(pool_type *pool) { \
	unsigned int index; \
	if (pool->free_list_length > 0) { \
		index = pool->free_list[--pool->free_list_length]; \
	} else if (pool->count < (pool->chunk_count * chunk_size)) { \
		index = pool->count;\
	} else { \
		index = pool->count;\
		pool->chunks = realloc(pool->chunks, sizeof(element_type*) * (pool->chunk_count + 1));\
		pool->chunks[pool->chunk_count] = calloc(sizeof(element_type) * chunk_size, 1);\
		pool->free_list = realloc(pool->free_list, sizeof(unsigned int) * (pool->chunk_count + 1) * chunk_size);\
		++pool->chunk_count;\
	} \
	++pool->count;\
	unsigned int generation = ++pool->chunks[index / chunk_size][index % chunk_size].generation;\
	return (handle_type) { .index = index, generation = generation };\
} \
__unused static void CONCAT(function_name, _release)(pool_type *pool, handle_type handle) { \
	element_type* e = pool->chunks[handle.index / chunk_size] + (handle.index % chunk_size);\
	if (e->generation != handle.generation) {\
		return;\
	}\
	pool->free_list[pool->free_list_length++] = handle.index; \
	memset(e, 0, sizeof(element_type)); \
	e->generation = handle.generation + 1;\
	--pool->count; \
} \
__unused static element_type* CONCAT(function_name, _deref)(pool_type *pool, handle_type handle) { \
	element_type* e = pool->chunks[handle.index / chunk_size] + (handle.index % chunk_size);\
	if (e->generation == handle.generation) {\
		return e;\
	} else {\
		return NULL;\
	}\
} \
