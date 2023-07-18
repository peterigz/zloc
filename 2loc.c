#include <math.h>
#include "header.h"

//Header
#define tloc__Min(a, b) (((a) < (b)) ? (a) : (b))
#define tloc__Max(a, b) (((a) > (b)) ? (a) : (b))

typedef unsigned int TLocIndex;
typedef unsigned int u32;
typedef int bool;
typedef ptrdiff_t TLocDiff;

#ifndef MEMORY_ALIGNMENT_LOG2
#define MEMORY_ALIGNMENT_LOG2 2		//32 bit
#endif

#define MIN_BLOCK_SIZE 16

enum tloc__constants {
	tloc__MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
	tloc__MINIMUM_BLOCK_SIZE = 16,
	tloc__FIRST_LEVEL_INDEX_MAX = 31,
	tloc__BLOCK_IS_FREE = 1 << 0,
	tloc__PREV_BLOCK_IS_FREE = 1 << 1,
	tloc__BLOCK_POINTER_OFFSET = sizeof(void*) + sizeof(u32),
	tloc__BLOCK_SIZE_OVERHEAD = sizeof(u32),
	tloc__POINTER_SIZE = sizeof(void*)
};

typedef enum tloc__error_codes {
	tloc__OK,
	tloc__INVALID_FIRST_BLOCK,
	tloc__INVALID_BLOCK_FOUND,
	tloc__INVALID_SEGRATED_LIST
} tloc__error_codes;

typedef struct TLocHeader {
	struct TLocHeader *prev_physical_block;
	u32 size;
	struct TLocHeader *prev_free_block;
	struct TLocHeader *next_free_block;
} TLocHeader;

typedef struct TLoc {
	TLocHeader terminator_block;
	u32 second_level_index;
	TLocHeader *first_block;
	size_t total_memory;
	u32 first_level_bitmap;
	u32 *second_level_bitmaps;
	TLocHeader ***segregated_lists;
} TLoc;

TLoc *tloc_InitialiseAllocator(void *memory, u32 size, u32 second_level_index);
void *tloc_Allocate(TLoc *allocator, u32 size);
void tloc_Free(TLoc *allocator, void *allocation);

//Debugging
tloc__error_codes tloc_VerifyBlocks(TLoc *allocator);
tloc__error_codes tloc_VerifySegregatedLists(TLoc *allocator);
bool tloc_ValidPointer(TLoc *allocator, void *pointer);
bool tloc_ValidBlock(TLoc *allocator, TLocHeader *block);
//----

//Private inline functions
static inline void tloc__map(u32 size, u32 sub_div_log2, u32 *fli, u32 *sli) {
	_BitScanReverse(fli, size);
	size = size & ~(1 << *fli);
	*sli = size >> (*fli - sub_div_log2);
}

static inline bool tloc__has_free_block(TLoc *allocator, u32 fli, u32 sli) {
	return allocator->first_level_bitmap & (1U << fli) && allocator->second_level_bitmaps[fli] & (1U << sli);
}

static inline void tloc__flag_block_as_free(TLocHeader *block) {
	block->size |= tloc__BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_used(TLocHeader *block) {
	block->size &= ~tloc__BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_prev_free(TLocHeader *block) {
	block->size |= tloc__PREV_BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_prev_used(TLocHeader *block) {
	block->size &= ~tloc__PREV_BLOCK_IS_FREE;
}

static inline bool tloc__is_free_block(TLocHeader *block) {
	return block->size & tloc__BLOCK_IS_FREE;
}

static inline bool tloc__prev_is_free_block(TLocHeader *block) {
	return block->size & tloc__PREV_BLOCK_IS_FREE;
}

static inline bool tloc__is_aligned(u32 size, u32 alignment) {
	return (size % alignment) == 0;
}

static inline u32 tloc__align_size_down(u32 size, u32 alignment) {
	return size - (size % alignment);
}

static inline u32 tloc__align_size_up(u32 size, u32 alignment) {
	u32 remainder = size % alignment;
	if (remainder != 0) {
		size += alignment - remainder;
	}
	return size;
}

static inline void *tloc__end_of_memory_pointer(TLoc *allocator) {
	return (void*)((char*)allocator + allocator->total_memory);
}

static inline TLocHeader *tloc__first_block(TLoc *allocator) {
	return allocator->first_block;
}

static inline u32 tloc__block_size(TLocHeader *block) {
	return block->size & 0xFFFFFFFC;
}

static inline void tloc__set_block_size(TLocHeader *block, u32 size) {
	u32 boundary_tag = block->size & 0x00000003;
	block->size = size | boundary_tag;
}

static inline void tloc__set_prev_physical_block(TLocHeader *block, TLocHeader *prev_block) {
	block->prev_physical_block = prev_block;
}

static inline TLocHeader *tloc__block_from_allocation(void *allocation) {
	return (TLocHeader*)((char*)allocation - tloc__BLOCK_POINTER_OFFSET);
}

static inline TLocHeader *tloc__terminate(TLoc *allocator) {
	return &allocator->terminator_block;
}

static inline void tloc__push_block(TLoc *allocator, TLocHeader *block) {

	TLocIndex fi;
	TLocIndex si;
	tloc__map(tloc__block_size(block), allocator->second_level_index, &fi, &si);
	TLocHeader *current_block_in_free_list = allocator->segregated_lists[fi][si];
	tloc__flag_block_as_free(block);
	if (current_block_in_free_list == tloc__terminate(allocator)) {
		int d = 0;
	}
	block->next_free_block = current_block_in_free_list;
	block->prev_free_block = &allocator->terminator_block;
	//This might be the terminator block but it doesn't matter?
	current_block_in_free_list->prev_free_block = block;

	allocator->segregated_lists[fi][si] = block;
	allocator->first_level_bitmap |= 1 << fi;
	allocator->second_level_bitmaps[fi] |= 1 << si;

}

static inline TLocHeader *tloc__pop_block(TLoc *allocator, u32 fli, u32 sli) {
	TLocHeader *block = allocator->segregated_lists[fli][sli];
	assert(block != &allocator->terminator_block);
	if (block->next_free_block != &allocator->terminator_block) {
		allocator->segregated_lists[fli][sli] = block->next_free_block;
		allocator->segregated_lists[fli][sli]->prev_free_block = tloc__terminate(allocator);
	}
	else {
		allocator->segregated_lists[fli][sli] = tloc__terminate(allocator);
		allocator->second_level_bitmaps[fli] &= ~(1 << sli);
		if (allocator->second_level_bitmaps[fli] == 0) {
			allocator->first_level_bitmap &= ~(1 << fli);
		}
	}
	return block;
}

static inline void tloc__free_block(TLoc *allocator, TLocHeader *block) {
	u32 fli, sli;
	tloc__map(tloc__block_size(block), allocator->second_level_index, &fli, &sli);
	TLocHeader *prev_block = block->prev_free_block;
	TLocHeader *next_block = block->next_free_block;
	assert(prev_block);
	assert(next_block);
	next_block->prev_free_block = prev_block;
	prev_block->next_free_block = next_block;
	if (allocator->segregated_lists[fli][sli] == block) {
		allocator->segregated_lists[fli][sli] = next_block;
		if (next_block == tloc__terminate(allocator)) {
			allocator->second_level_bitmaps[fli] &= ~(1U << sli);
			if (allocator->second_level_bitmaps[fli] == 0) {
				allocator->first_level_bitmap &= ~(1U << fli);
			}
		}
	}
}

static inline TLocIndex tloc__find_next_size_up(u32 map, u32 start) {
	TLocIndex scan_result = 0;
	map &= ~(1U << start);
	_BitScanForward(&scan_result, map);
	return scan_result;
}

static inline void* tloc__block_user_ptr(TLocHeader *block) {
	return (char*)block + tloc__BLOCK_POINTER_OFFSET;
}

static inline TLocHeader *tloc__next_physical_block(TLocHeader *block) {
	return (TLocHeader*)((char*)tloc__block_user_ptr(block) + tloc__block_size(block));
}

static inline bool tloc__is_last_block(TLoc *allocator, TLocHeader *block) {
	return (void*)tloc__next_physical_block(block) == tloc__end_of_memory_pointer(allocator);
}

static inline void *tloc__split_block(TLoc *allocator, TLocHeader *block, u32 size) {
	u32 size_plus_overhead = size + tloc__BLOCK_POINTER_OFFSET;
	TLocHeader *trimmed = (TLocHeader*)((char*)tloc__block_user_ptr(block) + size);
	trimmed->size = 0;
	tloc__set_block_size(trimmed, tloc__block_size(block) - size_plus_overhead);
	tloc__set_prev_physical_block(trimmed, block);
	tloc__push_block(allocator, trimmed);
	tloc__set_block_size(block, size);
	tloc__flag_block_as_used(block);
	return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
}

static inline void tloc__merge_with_prev_block(TLoc *allocator, TLocHeader *block) {
	TLocHeader *prev_block = block->prev_physical_block;
	tloc__set_block_size(prev_block, tloc__block_size(prev_block) + tloc__block_size(block));
	tloc__free_block(allocator, prev_block);
	block = prev_block;
}

static inline void tloc__merge_with_next_block_if_free(TLoc *allocator, TLocHeader *block) {
	TLocHeader *next_block = tloc__next_physical_block(block);
	if (tloc__is_free_block(next_block)) {
		tloc__set_block_size(block, tloc__block_size(next_block) + tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET);
		tloc__free_block(allocator, next_block);
	}
}

//--- End Header

//--- Debugging tools
bool tloc_ValidPointer(TLoc *allocator, void *pointer) {
	return pointer >= (void*)allocator && pointer <= (void*)((char*)allocator + allocator->total_memory);
}

bool tloc_ValidBlock(TLoc *allocator, TLocHeader *block) {
	if (tloc_ValidPointer(allocator, (void*)block)) {
		return tloc_ValidPointer(allocator, (void*)block->prev_physical_block);
	}
	return 0;
}

tloc__error_codes tloc_VerifyBlocks(TLoc *allocator) {
	TLocHeader *current_block = allocator->first_block;
	if (!tloc_ValidPointer(allocator, (void*)current_block)) {
		return tloc__INVALID_FIRST_BLOCK;
	}
	void *end_pointer = tloc__end_of_memory_pointer(allocator);
	while (!tloc__is_last_block(allocator, current_block)) {
		if (!tloc_ValidBlock(allocator, current_block)) {
			return current_block == allocator->first_block ? tloc__INVALID_FIRST_BLOCK : tloc__INVALID_BLOCK_FOUND;
		}
		current_block = tloc__next_physical_block(current_block);
	}
	return tloc__OK;
}

tloc__error_codes tloc_VerifySegregatedLists(TLoc *allocator) {
	TLocIndex scan_result;
	_BitScanReverse(&scan_result, (u32)allocator->total_memory);
	u32 first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX);
	u32 second_level_index_count = 1 << allocator->second_level_index;
	for (int fli = 0; fli != first_level_index_count; ++fli) {
		for (int sli = 0; sli != second_level_index_count; ++sli) {
			TLocHeader *block = allocator->segregated_lists[fli][sli];
			if (block == tloc__terminate(allocator)) {
				continue;
			} else if (!tloc_ValidBlock(allocator, allocator->segregated_lists[fli][sli])) {
				return tloc__INVALID_SEGRATED_LIST;
			}
		}
	}
	return tloc__OK;
}

//--- End Debugging tools

//Definitions
TLoc *tloc_InitialiseAllocator(void *memory, u32 size, u32 second_level_index) {
	assert(tloc__is_aligned(size, tloc__MEMORY_ALIGNMENT));
	TLoc *allocator = (TLoc*)memory;
	memset(allocator, 0, sizeof(TLoc));
	allocator->terminator_block.next_free_block = &allocator->terminator_block;
	allocator->terminator_block.prev_free_block = &allocator->terminator_block;
	allocator->second_level_index = second_level_index;

	TLocIndex scan_result;
	//Get the number of first level size categorisations. Must not be larger then 31
	_BitScanReverse(&scan_result, size);
	u32 first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX);
	//Each first level size class then splits into finer classes by the second level index log2
	u32 second_level_index_count = 1 << second_level_index;

	//We store the lists containing pointers to the first free block for each of those category classed at the start of the memory
	//pool as well.
	//Calculate the size of the lists which is a two level array [first_level_index_count][second_level_index_count]
	//If the size of the pool is too small then assert
	//We need the size of the second level list to know how much to offset the pointer in the first level list
	u32 size_of_second_level_bitmap_list = second_level_index_count * sizeof(u32);
	u32 size_of_each_second_level_list = second_level_index_count * tloc__POINTER_SIZE;
	u32 size_of_first_level_list = first_level_index_count * tloc__POINTER_SIZE;
	u32 segregated_list_size = (second_level_index_count * tloc__POINTER_SIZE * first_level_index_count) + (first_level_index_count * tloc__POINTER_SIZE);
	u32 lists_size = segregated_list_size + size_of_second_level_bitmap_list;
	assert(size > lists_size + tloc__MINIMUM_BLOCK_SIZE);

	//Set the pointer to the start of the memory pool which starts after the segregated_lists array
	allocator->total_memory = size;
	size_t array_offset = sizeof(TLoc);
	allocator->first_block = (TLocHeader*)((char*)memory + array_offset + lists_size);
	allocator->second_level_bitmaps = (u32*)(((char*)memory) + array_offset);
	memset(allocator->second_level_bitmaps, 0, size_of_second_level_bitmap_list);
	//Point all of the segregated list array pointers to the empty block
	allocator->segregated_lists = (TLocHeader***)(((char*)memory) + array_offset + size_of_second_level_bitmap_list);
	for (u32 i = 0; i < first_level_index_count; i++) {
		TLocHeader **ptr = (TLocHeader**)((char*)memory + (array_offset + size_of_second_level_bitmap_list + size_of_first_level_list) + (i * size_of_each_second_level_list));
		allocator->segregated_lists[i] = ptr;
		for (u32 j = 0; j < second_level_index_count; j++) {
			allocator->segregated_lists[i][j] = &allocator->terminator_block;
		}
	}
	//Now add the memory pool into the segregated list as a free block.
	//Offset it back by the pointer size and set the previous physical block to the terminator block
	TLocHeader *block = allocator->first_block;
	tloc__set_block_size(block, size - ((u32)array_offset + lists_size + tloc__POINTER_SIZE + tloc__BLOCK_SIZE_OVERHEAD));

	//The size of the allocator + initial free memory should add up to the size of memory being used
	assert(tloc__block_size(block) + lists_size + array_offset + tloc__POINTER_SIZE + tloc__BLOCK_SIZE_OVERHEAD == size);
	//Make sure it aligns to nearest multiple of 4
	tloc__set_block_size(block, tloc__align_size_down(tloc__block_size(block), tloc__MEMORY_ALIGNMENT));
	assert(tloc__block_size(block) > tloc__MINIMUM_BLOCK_SIZE);
	tloc__flag_block_as_free(block);
	tloc__flag_block_as_prev_used(block);
	tloc__push_block(allocator, block);

	block->prev_physical_block = tloc__terminate(allocator);

	return allocator;
}

void *tloc_Allocate(TLoc *allocator, u32 size) {
	TLocIndex fli;
	TLocIndex sli;
	size = tloc__align_size_up(size, tloc__MEMORY_ALIGNMENT);
	tloc__map(size, allocator->second_level_index, &fli, &sli);
	if (tloc__has_free_block(allocator, fli, sli)) {
		return tloc__pop_block(allocator, fli, sli);
	}
	sli = tloc__find_next_size_up(allocator->second_level_bitmaps[fli], sli);
	if (!sli) {
		fli = tloc__find_next_size_up(allocator->first_level_bitmap, fli);
		if (fli) {
			_BitScanForward(&sli, allocator->second_level_bitmaps[fli]);
			TLocHeader *block = tloc__pop_block(allocator, fli, sli);
			void *allocation = tloc__split_block(allocator, block, size);
			return allocation;
		}
	}
	//Out of memory;
	return NULL;
}

void tloc_Free(TLoc *allocator, void* allocation) {
	TLocHeader *block = (TLocHeader*)((char*)allocation - tloc__BLOCK_POINTER_OFFSET);
	if (tloc__prev_is_free_block(block)) {
		assert(block->prev_physical_block);		//Must be a valid previous physical block
		tloc__merge_with_prev_block(allocator, block);
		//Now block = the prev block
	}
	tloc__merge_with_next_block_if_free(allocator, block);
	tloc__push_block(allocator, block);
}

int main()
{

	void *memory = malloc(1024 * 1024 * 16);
	TLoc *allocator = tloc_InitialiseAllocator(memory, 1024 * 1024 * 16, 5);
	int *allocation = tloc_Allocate(allocator, 1024);
	for (int c = 0; c != 256; ++c) {
		allocation[c] = c;
	}
	for (int c = 0; c != 1024 / sizeof(int); ++c) {
		printf("%i\n", allocation[c]);
	}
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	assert(tloc_VerifyBlocks(allocator) == tloc__OK);
	tloc_Free(allocator, allocation);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	assert(tloc_VerifyBlocks(allocator) == tloc__OK);

	free(memory);
	
}
