// TLSFAllocator.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <math.h>
#include "header.h"

//Header
#define tfxKilobyte(Value) ((Value)*1024LL)
#define tfxMegabyte(Value) (tfxKilobyte(Value)*1024LL)
#define tfxGigabyte(Value) (tfxMegabyte(Value)*1024LL)
#define tfxMin(a, b) (((a) < (b)) ? (a) : (b))
#define tfxMax(a, b) (((a) > (b)) ? (a) : (b))

typedef unsigned int TLocIndex;
typedef unsigned int u32;
typedef int bool;

#ifndef MEMORY_ALIGNMENT_LOG2
#define MEMORY_ALIGNMENT_LOG2 3
#endif

#define MIN_BLOCK_SIZE 16

enum tloc_constants {
	MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
	MINIMUM_BLOCK_SIZE = 16,
	FIRST_LEVEL_INDEX_MAX = 31,
	BLOCK_IS_FREE = 1 << 0,
	PREV_BLOCK_IS_FREE = 1 << 1,
	USED_BLOCK_OVERHEAD = sizeof(u32),
	BLOCK_START_OFFSET = sizeof(u32),
	POINTER_SIZE = sizeof(void*)
};

typedef struct TLocHeader {
	struct TLocHeader *prev_physical_block;
	u32 size;
	struct TLocHeader *prev_free_block;
	struct TLocHeader *next_free_block;
} TLocHeader;

typedef struct TLoc {
	TLocHeader terminator_block;
	u32 second_level_index;
	u32 first_level_index_count;
	u32 second_level_index_count;
	u32 first_level_bitmap;
	void *start_of_memory_pool;
	size_t total_size_of_available_memory;
	u32 *second_level_bitmaps;
	TLocHeader ***segregated_lists;
} TLoc;

TLoc *tloc_InitialiseAllocator(void *memory, u32 size, u32 second_level_index);
void *tloc_Allocate(TLoc *allocator, u32 size);
void tloc_Free(TLoc *allocator, void *allocation);

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
	block->size |= BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_used(TLocHeader *block) {
	block->size &= ~BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_prev_free(TLocHeader *block) {
	block->size |= PREV_BLOCK_IS_FREE;
}

static inline void tloc__flag_block_as_prev_used(TLocHeader *block) {
	block->size &= ~PREV_BLOCK_IS_FREE;
}

static inline bool tloc__is_free_block(TLocHeader *block) {
	return block->size & BLOCK_IS_FREE;
}

static inline bool tloc__prev_is_free_block(TLocHeader *block) {
	return block->size & PREV_BLOCK_IS_FREE;
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

static inline u32 tloc__block_size(TLocHeader *block) {
	return block->size & 0xFFFFFFFC;
}

static inline TLocHeader *tloc__terminate(TLoc *allocator) {
	return &allocator->terminator_block;
}

static inline void tloc__push_block(TLoc *allocator, TLocHeader *block) {

	TLocIndex fi;
	TLocIndex si;
	tloc__map(tloc__block_size(block), allocator->second_level_index, &fi, &si);
	TLocHeader *current_block_in_free_list = allocator->segregated_lists[fi][si];
	block->next_free_block = current_block_in_free_list;
	block->prev_free_block = &allocator->terminator_block;
	tloc__flag_block_as_free(block);
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
		allocator->second_level_bitmaps[fli] &= ~(1 << sli);
	}
	return block;
}

static inline void tloc__free_block(TLoc *allocator, TLocHeader *block) {

}

static inline TLocIndex tloc__find_next_size_up(u32 map, u32 start) {
	TLocIndex scan_result = 0;
	map &= ~(1U << start);
	_BitScanForward(&scan_result, map);
	return scan_result;
}

static inline void *tloc__split_block(TLoc *allocator, TLocHeader *block, u32 size) {
	u32 size_plus_overhead = size + USED_BLOCK_OVERHEAD;
	TLocHeader *trimmed = (TLocHeader*)((char*)block + size_plus_overhead);
	trimmed->size = block->size - size_plus_overhead;
	trimmed->size = tloc__align_size_down(trimmed->size, MEMORY_ALIGNMENT);
	trimmed->prev_physical_block = block;
	tloc__push_block(allocator, trimmed);
	block->size = size_plus_overhead;
	tloc__flag_block_as_used(block);
	return (char*)(block + BLOCK_START_OFFSET);
}

static inline void tloc__merge_block(TLoc *allocator, TLocHeader *block) {
	//Is the previous block free?
	TLocHeader *prev_block = block->prev_physical_block;
	if (prev_block) {
	}
}
//--- End Header

//Definitions
TLoc *tloc_InitialiseAllocator(void *memory, u32 size, u32 second_level_index) {
	TLoc *allocator = (TLoc*)memory;
	memset(allocator, 0, sizeof(TLoc));
	allocator->terminator_block.next_free_block = &allocator->terminator_block;
	allocator->terminator_block.prev_free_block = &allocator->terminator_block;
	allocator->second_level_index = second_level_index;

	TLocIndex scan_result;
	//Get the number of first level size categorisations. Must not be larger then 31
	_BitScanReverse(&scan_result, size);
	allocator->first_level_index_count = tfxMin(scan_result, FIRST_LEVEL_INDEX_MAX);
	//Each first level size class then splits into finer classes by the second level index log2
	allocator->second_level_index_count = 1 << second_level_index;

	//We store the lists containing pointers to the first free block for each of those category classed at the start of the memory
	//pool as well.
	//Calculate the size of the lists which is a two level array [first_level_index_count][second_level_index_count]
	//If the size of the pool is too small then assert
	//We need the size of the second level list to know how much to offset the pointer in the first level list
	u32 size_of_second_level_bitmap_list = allocator->second_level_index_count * sizeof(u32);
	u32 size_of_each_second_level_list = allocator->second_level_index_count * sizeof(TLocHeader**);
	u32 size_of_first_level_list = allocator->first_level_index_count * sizeof(TLocHeader***);
	u32 lists_size = allocator->first_level_index_count * allocator->second_level_index_count * sizeof(TLocHeader*) + size_of_second_level_bitmap_list;
	assert(size > lists_size + MINIMUM_BLOCK_SIZE);
	//Set the pointer to the start of the memory pool which starts after the segregated_lists array
	allocator->start_of_memory_pool = (char*)memory + sizeof(TLoc) + lists_size;
	size_t array_offset = sizeof(TLoc);
	allocator->second_level_bitmaps = (u32*)(((char*)memory) + array_offset);
	memset(allocator->second_level_bitmaps, 0, size_of_second_level_bitmap_list);
	//Point all of the segregated list array pointers to the empty block
	allocator->segregated_lists = (TLocHeader***)(((char*)memory) + array_offset + size_of_second_level_bitmap_list);
	for (u32 i = 0; i < allocator->first_level_index_count; i++) {
		TLocHeader **ptr = (TLocHeader**)((char*)memory + (array_offset + size_of_second_level_bitmap_list + size_of_first_level_list) + (i * size_of_each_second_level_list));
		allocator->segregated_lists[i] = ptr;
		for (u32 j = 0; j < allocator->second_level_index_count; j++) {
			allocator->segregated_lists[i][j] = &allocator->terminator_block;
		}
	}
	//Now add the memory pool into the segregated list as a free block
	TLocHeader *block = (TLocHeader*)allocator->start_of_memory_pool;
	block->size = size - ((u32)array_offset + lists_size) - USED_BLOCK_OVERHEAD;
	//Make sure it aligns to nearest multiple of 4
	block->size = tloc__align_size_down(block->size, MEMORY_ALIGNMENT);
	assert(block->size > MINIMUM_BLOCK_SIZE);
	allocator->total_size_of_available_memory = block->size;
	tloc__flag_block_as_free(block);
	tloc__flag_block_as_prev_used(block);
	tloc__push_block(allocator, block);

	return allocator;
}

void *tloc_Allocate(TLoc *allocator, u32 size) {
	TLocIndex fli;
	TLocIndex sli;
	size = tloc__align_size_up(size, MEMORY_ALIGNMENT);
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
	TLocHeader *block = ((char*)allocation - USED_BLOCK_OVERHEAD);
	if (tloc__prev_is_free_block(block)) {
		assert(block->prev_physical_block);		//Must be a valid previous physical block
		//merge with the prev free block
	}
}

int main()
{

	void *memory = malloc(tfxMegabyte(16));
	TLoc *allocator = tloc_InitialiseAllocator(memory, tfxMegabyte(16), 5);
	void *allocation = tloc_Allocate(allocator, 1024);

	for (int i = 0; i != allocator->first_level_index_count; ++i) {
		for (int j = 0; j != allocator->second_level_index_count; ++j) {
			printf("%d, %d, %p \n", i, j, (void*)allocator->segregated_lists[i][j]);
		}
	}

	free(memory);
	
}
