#ifndef TLOC_INCLUDE_H
#define TLOC_INCLUDE_H

//#define TLOC_DEV_MODE

#include <stdlib.h>
#include <assert.h>

//Header
#define tloc__Min(a, b) (((a) < (b)) ? (a) : (b))
#define tloc__Max(a, b) (((a) > (b)) ? (a) : (b))

typedef int tloc_index;
typedef size_t tloc_size;
typedef size_t tloc_fl_bitmap;
typedef unsigned int tloc_sl_bitmap;
typedef unsigned int tloc_uint;
typedef int tloc_bool;

#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__x86_64__)
#define tloc__64BIT
#endif

#ifndef MEMORY_ALIGNMENT_LOG2
#if defined(tloc__64BIT)
#define MEMORY_ALIGNMENT_LOG2 3		//64 bit
#else
#define MEMORY_ALIGNMENT_LOG2 2		//32 bit
#endif
#endif

#ifndef TLOC_ERROR_NAME
#define TLOC_ERROR_NAME "Allocator Error"
#endif

#ifdef TLOC_OUTPUT_ERROR_MESSAGES
#define TLOC_PRINT_ERROR(message_f, ...) printf(message_f, __VA_ARGS__)
#else
#define TLOC_PRINT_ERROR(message_f, ...)
#endif

#define MIN_BLOCK_SIZE 16

#ifdef __cplusplus
extern "C" {
#endif

enum tloc__constants {
	tloc__MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
	tloc__MINIMUM_BLOCK_SIZE = 16,
	tloc__FIRST_LEVEL_INDEX_MAX = (1 << (MEMORY_ALIGNMENT_LOG2 + 3)) - 1,
	tloc__BLOCK_IS_FREE = 1 << 0,
	tloc__PREV_BLOCK_IS_FREE = 1 << 1,
	tloc__BLOCK_POINTER_OFFSET = sizeof(void*) + sizeof(tloc_size),
	tloc__BLOCK_SIZE_OVERHEAD = sizeof(tloc_size),
	tloc__POINTER_SIZE = sizeof(void*)
};

typedef enum tloc__error_codes {
	tloc__OK,
	tloc__INVALID_FIRST_BLOCK,
	tloc__INVALID_BLOCK_FOUND,
	tloc__INVALID_SEGRATED_LIST,
	tloc__SECOND_LEVEL_BITMAPS_NOT_INITIALISED
} tloc__error_codes;

typedef struct tloc_header {
	struct tloc_header *prev_physical_block;
	tloc_size size;
	struct tloc_header *prev_free_block;
	struct tloc_header *next_free_block;
} tloc_header;

typedef struct tloc_allocator {
	tloc_header end_block;
	tloc_index second_level_index;
	tloc_header *first_block;
	void *end_of_pool;
	size_t total_memory;
	tloc_fl_bitmap first_level_bitmap;
	tloc_sl_bitmap *second_level_bitmaps;
	tloc_header ***segregated_lists;
} tloc_allocator;

#if defined (_MSC_VER) && (_MSC_VER >= 1400) && (defined (_M_IX86) || defined (_M_X64))
/* Microsoft Visual C++ support on x86/X64 architectures. */

#include <intrin.h>

static inline int tloc__scan_reverse(tloc_size bitmap)
{
	unsigned long index;
#if defined(tloc__64BIT)
	return _BitScanReverse64(&index, bitmap) ? index : -1;
#else
	return _BitScanReverse(&index, bitmap) ? index : -1;
#endif
}

static inline int tloc__scan_forward(tloc_size bitmap)
{
	unsigned long index;
#if defined(tloc__64BIT)
	return _BitScanForward64(&index, bitmap) ? index : -1;
#else
	return _BitScanForward(&index, bitmap) ? index : -1;
#endif
}

#elif defined(__GNUC__) && ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)) && \
      (defined(__i386__) || defined(__x86_64__)) || defined(__clang__)
/* GNU C/C++ or Clang support on x86/x64 architectures. */

static inline int tloc__scan_reverse(tloc_size bitmap)
{
#if defined(tloc__64BIT)
    return 64 - __builtin_clzll(bitmap) - 1;
#else
    return 32 - __builtin_clz((int)bitmap) - 1;
#endif
}

static inline int tloc__scan_forward(tloc_size bitmap)
{
#if defined(tloc__64BIT)
    return __builtin_ffsll(bitmap) - 1;
#else
    return __builtin_ffs((int)bitmap) - 1;
#endif
}

#endif

//Private inline functions, user doesn't need to call these
static inline void tloc__map(tloc_size size, tloc_uint sub_div_log2, tloc_index *fli, tloc_index *sli, tloc_index max_sli) {
	*fli = tloc__scan_reverse(size);
	size = size & ~(1 << *fli);
	*sli = (tloc_index)(size >> (*fli - sub_div_log2)) % max_sli;
}

static inline tloc_bool tloc__has_free_block(tloc_allocator *allocator, tloc_index fli, tloc_index sli) {
	return allocator->first_level_bitmap & (1ULL << fli) && allocator->second_level_bitmaps[fli] & (1U << sli);
}

static inline void tloc__clear_boundary_tag(tloc_header *block) {
	block->size &= ~(tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE);
}

static inline void tloc__flag_block_as_free(tloc_header *block) {
	block->size |= tloc__BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_used(tloc_header *block) {
	block->size &= ~tloc__BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_prev_free(tloc_header *block) {
	block->size |= tloc__PREV_BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_prev_used(tloc_header *block) {
	block->size &= ~tloc__PREV_BLOCK_IS_FREE;
}

static inline tloc_bool tloc__is_free_block(tloc_header *block) {
	return block->size & tloc__BLOCK_IS_FREE;
}

static inline tloc_bool tloc__prev_is_free_block(tloc_header *block) {
	return block->size & tloc__PREV_BLOCK_IS_FREE;
}

static inline tloc_bool tloc__is_aligned(tloc_size size, tloc_index alignment) {
	return (size % alignment) == 0;
}

static inline tloc_size tloc__align_size_down(tloc_size size, tloc_index alignment) {
	return size - (size % alignment);
}

static inline tloc_size tloc__align_size_up(tloc_size size, tloc_index alignment) {
	tloc_size remainder = size % alignment;
	if (remainder != 0) {
		size += alignment - remainder;
	}
	return size;
}

static inline void *tloc__end_of_memory_pointer(tloc_allocator *allocator) {
	return (void*)((char*)allocator + allocator->total_memory);
}

static inline tloc_header *tloc__first_block(tloc_allocator *allocator) {
	return allocator->first_block;
}

static inline tloc_size tloc__block_size(tloc_header *block) {
	return block->size & ~(tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE);
}

static inline void tloc__set_block_size(tloc_header *block, tloc_size size) {
	tloc_size boundary_tag = block->size & (tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE);
	block->size = size | boundary_tag;
}

static inline void tloc__set_prev_physical_block(tloc_header *block, tloc_header *prev_block) {
	block->prev_physical_block = prev_block;
}

static inline tloc_header *tloc__block_from_allocation(void *allocation) {
	return (tloc_header*)((char*)allocation - tloc__BLOCK_POINTER_OFFSET);
}

static inline tloc_header *tloc__end(tloc_allocator *allocator) {
	return &allocator->end_block;
}

static inline void* tloc__block_user_ptr(tloc_header *block) {
	return (char*)block + tloc__BLOCK_POINTER_OFFSET;
}

static inline tloc_header *tloc__next_physical_block(tloc_header *block) {
	return (tloc_header*)((char*)tloc__block_user_ptr(block) + tloc__block_size(block));
}

static inline tloc_bool tloc__is_last_block(tloc_allocator *allocator, tloc_header *block) {
	return (void*)tloc__next_physical_block(block) == allocator->end_of_pool;
}

static inline void tloc__push_block(tloc_allocator *allocator, tloc_header *block) {

	tloc_index fi;
	tloc_index si;
	tloc__map(tloc__block_size(block), allocator->second_level_index, &fi, &si, 1 << allocator->second_level_index);
	tloc_header *current_block_in_free_list = allocator->segregated_lists[fi][si];
	tloc__flag_block_as_free(block);
	block->next_free_block = current_block_in_free_list;
	block->prev_free_block = &allocator->end_block;
	//This might be the terminator block but it doesn't matter?
	current_block_in_free_list->prev_free_block = block;

	allocator->segregated_lists[fi][si] = block;
	allocator->first_level_bitmap |= 1ULL << fi;
	allocator->second_level_bitmaps[fi] |= 1 << si;

	//let the next adjacent physical block know that this block is free
	if (!tloc__is_last_block(allocator, block)) {
		tloc__flag_block_as_prev_free(tloc__next_physical_block(block));
	}

}

static inline tloc_header *tloc__pop_block(tloc_allocator *allocator, tloc_index fli, tloc_index sli) {
	tloc_header *block = allocator->segregated_lists[fli][sli];
	assert(block != &allocator->end_block);
	if (block->next_free_block != &allocator->end_block) {
		allocator->segregated_lists[fli][sli] = block->next_free_block;
		allocator->segregated_lists[fli][sli]->prev_free_block = tloc__end(allocator);
	}
	else {
		allocator->segregated_lists[fli][sli] = tloc__end(allocator);
		allocator->second_level_bitmaps[fli] &= ~(1 << sli);
		if (allocator->second_level_bitmaps[fli] == 0) {
			allocator->first_level_bitmap &= ~(1 << fli);
		}
	}
	return block;
}

static inline void tloc__free_block(tloc_allocator *allocator, tloc_header *block) {
	tloc_index fli, sli;
	tloc__map(tloc__block_size(block), allocator->second_level_index, &fli, &sli, 1 << allocator->second_level_index);
	tloc_header *prev_block = block->prev_free_block;
	tloc_header *next_block = block->next_free_block;
	assert(prev_block);
	assert(next_block);
	next_block->prev_free_block = prev_block;
	prev_block->next_free_block = next_block;
	if (allocator->segregated_lists[fli][sli] == block) {
		allocator->segregated_lists[fli][sli] = next_block;
		if (next_block == tloc__end(allocator)) {
			allocator->second_level_bitmaps[fli] &= ~(1U << sli);
			if (allocator->second_level_bitmaps[fli] == 0) {
				allocator->first_level_bitmap &= ~(1U << fli);
			}
		}
	}
}

static inline tloc_index tloc__find_next_size_up(tloc_fl_bitmap map, tloc_uint start) {
	map &= ~(1ULL << start);
	return tloc__scan_forward(map);
}

static inline void *tloc__split_block(tloc_allocator *allocator, tloc_header *block, tloc_size size) {
	tloc_size size_plus_overhead = size + tloc__BLOCK_POINTER_OFFSET;
	tloc_header *trimmed = (tloc_header*)((char*)tloc__block_user_ptr(block) + size);
	trimmed->size = 0;
	tloc__set_block_size(trimmed, tloc__block_size(block) - size_plus_overhead);
	tloc__set_prev_physical_block(trimmed, block);
	tloc__push_block(allocator, trimmed);
	tloc__set_block_size(block, size);
	tloc__flag_block_as_used(block);
	return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
}

static inline tloc_header *tloc__merge_with_prev_block(tloc_allocator *allocator, tloc_header *block) {
	tloc_header *prev_block = block->prev_physical_block;
	tloc__set_block_size(prev_block, tloc__block_size(prev_block) + tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET);
	tloc__free_block(allocator, block);
	return prev_block;
}

static inline void tloc__merge_with_next_block_if_free(tloc_allocator *allocator, tloc_header *block) {
	tloc_header *next_block = tloc__next_physical_block(block);
	if (tloc__is_free_block(next_block)) {
		tloc__set_block_size(block, tloc__block_size(next_block) + tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET);
		tloc__free_block(allocator, next_block);
	}
}
//--End of internal functions

tloc_allocator *tloc_InitialiseAllocator(void *memory, tloc_size size, tloc_index second_level_index);
void *tloc_Allocate(tloc_allocator *allocator, tloc_size size);
void tloc_Free(tloc_allocator *allocator, void *allocation);

//Debugging
typedef void(*tloc__block_output)(void* ptr, size_t size, int used, void* user, int is_final_output);
tloc__error_codes tloc_VerifyBlocks(tloc_allocator *allocator, tloc__block_output output_function, void *user_data);
tloc__error_codes tloc_VerifySegregatedLists(tloc_allocator *allocator);
tloc__error_codes tloc_VerifySecondLevelBitmapsAreInitialised(tloc_allocator *allocator);
tloc_bool tloc_ValidPointer(tloc_allocator *allocator, void *pointer);
tloc_bool tloc_ValidBlock(tloc_allocator *allocator, tloc_header *block);
//--End of header declarations

#ifdef __cplusplus
}
#endif

#endif

//Implementation
#if defined(TLOC_IMPLEMENTATION) || defined(TLOC_DEV_MODE)

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

//Definitions
tloc_allocator *tloc_InitialiseAllocator(void *memory, tloc_size size, tloc_index second_level_index) {
	if (!memory) {
		TLOC_PRINT_ERROR("%s: The memory pointer passed in to the initialiser was NULL, did it allocate properly?\n", TLOC_ERROR_NAME);
		return 0;
	}
	if (!tloc__is_aligned(size, tloc__MEMORY_ALIGNMENT)) {
		TLOC_PRINT_ERROR("%s: memory passed to allocator is not aligned to: %u bytes\n", TLOC_ERROR_NAME, tloc__MEMORY_ALIGNMENT);
		return 0;
	}
	tloc_allocator *allocator = (tloc_allocator*)memory;
	memset(allocator, 0, sizeof(tloc_allocator));
	allocator->end_block.next_free_block = &allocator->end_block;
	allocator->end_block.prev_free_block = &allocator->end_block;
	allocator->second_level_index = second_level_index;
	size_t array_offset = sizeof(tloc_allocator);

	tloc_index scan_result;
	//Get the number of first level size categorisations. Must not be larger then 31
	scan_result = tloc__scan_reverse(size);
	tloc_uint first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	//Each first level size class then splits into finer classes by the second level index log2
	tloc_uint second_level_index_count = 1 << second_level_index;

	//We store the lists containing pointers to the first free block for each of those category classed at the start of the memory
	//pool as well.
	//Calculate the size of the lists which is a two level array [first_level_index_count][second_level_index_count]
	//If the size of the pool is too small then assert
	//We need the size of the second level list to know how much to offset the pointer in the first level list
	//There are first_level_index_counts of second_level_bitmaps as each first level set of size classes has it's own
	//set of finer size classes within that size.
	tloc_uint size_of_second_level_bitmap_list = first_level_index_count * sizeof(tloc_index);
	tloc_uint size_of_each_second_level_list = second_level_index_count * tloc__POINTER_SIZE;
	tloc_uint size_of_first_level_list = first_level_index_count * tloc__POINTER_SIZE;
	tloc_uint segregated_list_size = (second_level_index_count * tloc__POINTER_SIZE * first_level_index_count) + (first_level_index_count * tloc__POINTER_SIZE);
	tloc_uint lists_size = segregated_list_size + size_of_second_level_bitmap_list;
	size_t minimum_size = lists_size + tloc__MINIMUM_BLOCK_SIZE + array_offset + tloc__BLOCK_POINTER_OFFSET;
	if (size < minimum_size) {
		TLOC_PRINT_ERROR("%s: Tried to initialise allocator with a memory pool that is too small. Must be at least: %zi bytes\n", TLOC_ERROR_NAME, minimum_size);
		return 0;
	}

	//Set the pointer to the start of the memory pool which starts after the segregated_lists array
	allocator->total_memory = size;
	allocator->first_block = (tloc_header*)((char*)memory + array_offset + lists_size);
	allocator->second_level_bitmaps = (tloc_sl_bitmap*)(((char*)memory) + array_offset);
	memset(allocator->second_level_bitmaps, 0, size_of_second_level_bitmap_list);
	assert(tloc_VerifySecondLevelBitmapsAreInitialised(allocator) == tloc__OK);
	//Point all of the segregated list array pointers to the empty block
	allocator->segregated_lists = (tloc_header***)(((char*)memory) + array_offset + size_of_second_level_bitmap_list);
	for (tloc_uint i = 0; i < first_level_index_count; i++) {
		tloc_header **ptr = (tloc_header**)((char*)memory + (array_offset + size_of_second_level_bitmap_list + size_of_first_level_list) + (i * size_of_each_second_level_list));
		allocator->segregated_lists[i] = ptr;
		for (tloc_uint j = 0; j < second_level_index_count; j++) {
			allocator->segregated_lists[i][j] = &allocator->end_block;
		}
	}
	//Now add the memory pool into the segregated list as a free block.
	//Offset it back by the pointer size and set the previous physical block to the terminator block
	tloc_header *block = allocator->first_block;
	tloc__set_block_size(block, size - (array_offset + lists_size + tloc__POINTER_SIZE + tloc__BLOCK_SIZE_OVERHEAD));
	tloc__clear_boundary_tag(block);

	//The size of the allocator + initial free memory should add up to the size of memory being used
	assert(tloc__block_size(block) + lists_size + array_offset + tloc__POINTER_SIZE + tloc__BLOCK_SIZE_OVERHEAD == size);
	//Make sure it aligns to nearest multiple of 4
	tloc__set_block_size(block, tloc__align_size_down(tloc__block_size(block), tloc__MEMORY_ALIGNMENT));
	allocator->end_of_pool = tloc__next_physical_block(block);
	assert(tloc__block_size(block) > tloc__MINIMUM_BLOCK_SIZE);
	tloc__flag_block_as_free(block);
	tloc__flag_block_as_prev_used(block);
	tloc__push_block(allocator, block);

	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	assert(tloc__is_last_block(allocator, block));

	block->prev_physical_block = tloc__end(allocator);

	return allocator;
}

void *tloc_Allocate(tloc_allocator *allocator, tloc_size size) {
	tloc_index fli;
	tloc_index sli;
	size = tloc__align_size_up(size, tloc__MEMORY_ALIGNMENT);
	if (size < tloc__MINIMUM_BLOCK_SIZE) {
		TLOC_PRINT_ERROR("%s: Trying to allocate a block size that is too small. Minimum size is %u but trying to allocate %zu bytes\n", TLOC_ERROR_NAME, tloc__MINIMUM_BLOCK_SIZE, size);
		return NULL;
	}
	tloc__map(size, allocator->second_level_index, &fli, &sli, 1 << allocator->second_level_index);
	if (tloc__has_free_block(allocator, fli, sli)) {
		return tloc__pop_block(allocator, fli, sli);
	}
	sli = tloc__find_next_size_up(allocator->second_level_bitmaps[fli], sli);
	if (sli == -1) {
		fli = tloc__find_next_size_up(allocator->first_level_bitmap, fli);
		if (fli > -1) {
			sli = tloc__scan_forward(allocator->second_level_bitmaps[fli]);
			tloc_header *block = tloc__pop_block(allocator, fli, sli);
			if (block->size > size) {
				void *allocation = tloc__split_block(allocator, block, size);
				return allocation;
			}
		}
	}
	else {
		tloc_header *block = tloc__pop_block(allocator, fli, sli);
		if (block->size > size) {
			void *allocation = tloc__split_block(allocator, block, size);
			return allocation;
		}
	}
	//Out of memory;
	TLOC_PRINT_ERROR("%s: Not enough memory in pool to allocate %zu bytes\n", TLOC_ERROR_NAME, size);
	return NULL;
}

void tloc_Free(tloc_allocator *allocator, void* allocation) {
	tloc_header *block = (tloc_header*)((char*)allocation - tloc__BLOCK_POINTER_OFFSET);
	if (tloc__prev_is_free_block(block)) {
		tloc__push_block(allocator, block);
		assert(block->prev_physical_block);		//Must be a valid previous physical block
		block = tloc__merge_with_prev_block(allocator, block);
		//Now block = the prev block
	}
	tloc__merge_with_next_block_if_free(allocator, block);
	tloc__push_block(allocator, block);
}


//--- Debugging tools
tloc_bool tloc_ValidPointer(tloc_allocator *allocator, void *pointer) {
	return pointer >= (void*)allocator && pointer <= (void*)((char*)allocator + allocator->total_memory);
}

tloc_bool tloc_ValidBlock(tloc_allocator *allocator, tloc_header *block) {
	if (tloc_ValidPointer(allocator, (void*)block) && tloc_ValidPointer(allocator, (char*)block + sizeof(tloc_header))) {
		tloc_bool valid_prev_block = tloc_ValidPointer(allocator, (void*)block->prev_physical_block);
		tloc_bool valid_next_block = tloc_ValidPointer(allocator, tloc__next_physical_block(block));
		return valid_prev_block + valid_next_block;
	}
	return 0;
}

static void tloc__output(void* ptr, size_t size, int free, void* user, int is_final_output)
{
	(void)user;
	printf("\t%p %s size: %zi (%p)\n", ptr, free ? "free" : "used", size, ptr);
	if (is_final_output) {
		printf("\t------------- * ---------------\n");
	}
}

tloc__error_codes tloc_VerifyBlocks(tloc_allocator *allocator, tloc__block_output output_function, void *user_data) {
	tloc_header *current_block = allocator->first_block;
	if (!tloc_ValidPointer(allocator, (void*)current_block)) {
		return tloc__INVALID_FIRST_BLOCK;
	}
	while (!tloc__is_last_block(allocator, current_block)) {
		if (!tloc_ValidBlock(allocator, current_block)) {
			return current_block == allocator->first_block ? tloc__INVALID_FIRST_BLOCK : tloc__INVALID_BLOCK_FOUND;
		}
		if (output_function) {
			tloc__output(current_block, tloc__block_size(current_block), tloc__is_free_block(current_block), user_data, 0);
		}
		current_block = tloc__next_physical_block(current_block);
	}
	if (output_function) {
		tloc__output(current_block, tloc__block_size(current_block), tloc__is_free_block(current_block), user_data, 1);
	}
	return tloc__OK;
}

tloc__error_codes tloc_VerifySegregatedLists(tloc_allocator *allocator) {
	tloc_index scan_result;
	scan_result = tloc__scan_reverse(allocator->total_memory);
	tloc_index first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	tloc_index second_level_index_count = 1 << allocator->second_level_index;
	for (int fli = 0; fli != first_level_index_count; ++fli) {
		for (int sli = 0; sli != second_level_index_count; ++sli) {
			tloc_header *block = allocator->segregated_lists[fli][sli];
			if (block == tloc__end(allocator)) {
				continue;
			}
			else if (!tloc_ValidBlock(allocator, allocator->segregated_lists[fli][sli])) {
				return tloc__INVALID_SEGRATED_LIST;
			}
		}
	}
	return tloc__OK;
}

tloc__error_codes tloc_VerifySecondLevelBitmapsAreInitialised(tloc_allocator *allocator) {
	tloc_index scan_result;
	scan_result = tloc__scan_reverse(allocator->total_memory);
	tloc_index first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	for (int sli = 0; sli != first_level_index_count; ++sli) {
		if (allocator->second_level_bitmaps[sli] != 0) {
			return tloc__SECOND_LEVEL_BITMAPS_NOT_INITIALISED;
		}
	}
	return tloc__OK;
}

//--- End Debugging tools
#endif

#if defined(TLOC_DEV_MODE)
int main()
{
	tloc_size size = (1024ull * 1024ull * 1024ull * 6ull);	//6 gb
	for (tloc_size i = size; i < size * 6; i += (1024ull * 1024ull * 64ull)) {
		tloc_index fli, sli; 
		tloc__map(i, 5, &fli, &sli, 32);
		//printf("Size: %zu, fli: %i, sli: %i\n", i, fli, sli);
	}
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size, 5);
	assert(tloc_VerifyBlocks(allocator, tloc__output, 0) == tloc__OK);
	int *some_ints = tloc_Allocate(allocator, 1024);
	float *some_floats = tloc_Allocate(allocator, 4096);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	assert(tloc_VerifyBlocks(allocator, tloc__output, 0) == tloc__OK);
	tloc_Free(allocator, some_ints);
	assert(tloc_VerifyBlocks(allocator, tloc__output, 0) == tloc__OK);
	tloc_Free(allocator, some_floats);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	assert(tloc_VerifyBlocks(allocator, tloc__output, 0) == tloc__OK);
	free(memory);
}
#endif
