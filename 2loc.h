#ifndef TLOC_INCLUDE_H
#define TLOC_INCLUDE_H

#include <stdlib.h>
#include <assert.h>

//Header
#define tloc__Min(a, b) (((a) < (b)) ? (a) : (b))
#define tloc__Max(a, b) (((a) > (b)) ? (a) : (b))

#ifndef TLOC_API
#define TLOC_API
#endif

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

#ifndef TLOC_ERROR_COLOR
#define TLOC_ERROR_COLOR "\033[31m"
#endif

#ifdef TLOC_OUTPUT_ERROR_MESSAGES
#include <stdio.h>
#define TLOC_PRINT_ERROR(message_f, ...) printf(message_f"\033[0m", __VA_ARGS__)
#else
#define TLOC_PRINT_ERROR(message_f, ...)
#endif

#define MIN_BLOCK_SIZE 16
#define tloc__KILOBYTE(Value) ((Value) * 1024LL)
#define tloc__MEGABYTE(Value) (tloc__KILOBYTE(Value) * 1024LL)
#define tloc__GIGABYTE(Value) (tloc__MEGABYTE(Value) * 1024LL)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tloc__boundary_tag_flags {
	tloc__BLOCK_IS_FREE = 1 << 0,
	tloc__PREV_BLOCK_IS_FREE = 1 << 1,
} tloc__boundary_tag_flags;

enum tloc__constants {
	tloc__MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
	tloc__MINIMUM_BLOCK_SIZE = 16,
	tloc__SECOND_LEVEL_INDEX_LOG2 = 5,
	tloc__FIRST_LEVEL_INDEX_MAX = (1 << (MEMORY_ALIGNMENT_LOG2 + 3)) - 1,
	tloc__BLOCK_POINTER_OFFSET = sizeof(void*) + sizeof(tloc_size),
	tloc__BLOCK_SIZE_OVERHEAD = sizeof(tloc_size),
	tloc__POINTER_SIZE = sizeof(void*)
};

typedef enum tloc__error_codes {
	tloc__OK,
	tloc__INVALID_FIRST_BLOCK,
	tloc__INVALID_BLOCK_FOUND,
	tloc__PHYSICAL_BLOCK_MISALIGNMENT,
	tloc__INVALID_SEGRATED_LIST,
	tloc__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST,
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

/*
User functions
This are the main functions you'll need to use this library, everything else is either internal private functions or functions for debugging
*/

/*
	Initialise an allocator. Pass a block of memory that you want to use with the allocator, this can be created with malloc or VirtualAlloc
	or whatever your preference.
	Make sure the size matches the size of the memory you're passing

	@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
	@param	tloc_size				The size of the memory you're passing
	@returns tloc_allocator*		A pointer to a tloc_allocator which you'll need to use when calling tloc_Allocate or tloc_Free. Note that
									this pointer will be the same address as the memory you're passing in as all the information the allocator
									stores to organise memory blocks is stored at the beginning of the memory.
									If something went wrong then 0 is returned. Define TLOC_OUTPUT_ERROR_MESSAGES before including this header
									file to see any errors in the console.
*/
TLOC_API tloc_allocator *tloc_InitialiseAllocator(void *memory, tloc_size size);

/*
	A helper function to find the allocation size that takes into account the overhead of the allocator. So for example, let's say you want
	to allocate 128mb for a pool of memory, this function will take that size and return a new size that will include the extra overhead required
	by the allocator. The overhead inlcudes things like the first and second level bitmaps and the segregated list that stores pointers to free
	blocks. Note that the size will be rounded up to the nearest tloc__MEMORY_ALIGNMENT.

	@param	tloc_size				The size that you want the memory pool to be
	@returns tloc_size				The calculated size that will now include the required extra overhead for the allocator. You can now pass this
									size to tloc_InitialiseAllocator.
*/
TLOC_API tloc_size tloc_CalculateAllocationSize(tloc_size size);

/*
	Allocate some memory within a tloc_allocator of the specified size. Minimum size is 16 bytes. 

	@param	tloc_size				The size of the memory you're passing
	@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to 
									no free memory.
*/
TLOC_API void *tloc_Allocate(tloc_allocator *allocator, tloc_size size);

/*
	Free an allocation from a tloc_allocator. When freeing a block of memory any adjacent free blocks are merged together to keep on top of 
	fragmentation as much as possible. A check is also done to confirm that the block being freed is still valid and detect any memory corruption
	due to out of bounds writing of this or potentially other blocks.  

	It's recommended to call this function with an assert: assert(tloc_Free(allocator, allocation));
	An error is also output to console as long as TLOC_OUTPUT_ERROR_MESSAGES is defined.

	@returns int		returns 1 if the allocation was successfully freed, 0 otherwise.
*/
TLOC_API int tloc_Free(tloc_allocator *allocator, void *allocation);

/*
	Get the total size of the memory pool available for allocating after taking into account the allocator overhead

	@param tloc_allocator*			A pointer to a tcoc_allocator
	@returnst loc_size				This size of available pool memory
*/
TLOC_API tloc_size tloc_AllocatorPoolSize(tloc_allocator *allocator);

//--End of user functions

//Debugging
typedef void(*tloc__block_output)(void* ptr, size_t size, int used, void* user, int is_final_output);
//Loops through all blocks in the allocator and confirms that they all correctly link together
tloc__error_codes tloc_VerifyBlocks(tloc_allocator *allocator, tloc__block_output output_function, void *user_data);
//Makes sure that all blocks in the segregated list of free blocks are all valid
tloc__error_codes tloc_VerifySegregatedLists(tloc_allocator *allocator);
//All Second level bitmaps should be initialised to 0
tloc__error_codes tloc_VerifySecondLevelBitmapsAreInitialised(tloc_allocator *allocator);
//A valid pointer in the context of an allocator is one that points within the bounds of the memory assigned to the allocator
tloc_bool tloc_ValidPointer(tloc_allocator *allocator, void *pointer);
//Checks all pointers within the block are valid
tloc_bool tloc_ValidBlock(tloc_allocator *allocator, tloc_header *block);
//Checks to see if the block links up correctly with neighbouring blocks
tloc_bool tloc_ConfirmBlockLink(tloc_allocator *allocator, tloc_header *block);
//Search the segregated list of free blocks for a given block
tloc_bool tloc_BlockExistsInSegregatedList(tloc_allocator *allocator, tloc_header* block);
//The segregated list of free blocks should never contain any null blocks, this seeks them out.
tloc_bool tloc_CheckForNullBlocksInList(tloc_allocator *allocator);
//Get a count of all blocks in the allocator, whether or not they're free or used.
int tloc_BlockCount(tloc_allocator *allocator);

//Private inline functions, user doesn't need to call these
static inline void tloc__map(tloc_size size, tloc_uint sub_div_log2, tloc_index *fli, tloc_index *sli) {
	tloc_index max_sli = 1 << sub_div_log2;
	*fli = tloc__scan_reverse(size);
	size = size & ~(1 << *fli);
	*sli = (tloc_index)(size >> (*fli - sub_div_log2)) % max_sli;
}

static inline tloc_bool tloc__has_free_block(tloc_allocator *allocator, tloc_index fli, tloc_index sli) {
	return allocator->first_level_bitmap & (1ULL << fli) && allocator->second_level_bitmaps[fli] & (1U << sli);
}

static inline tloc_bool tloc__is_used_block(tloc_header *block) {
	return !(block->size & tloc__BLOCK_IS_FREE);
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

static inline void tloc__zero_block(tloc_header *block) {
	block->prev_physical_block = 0;
	block->size = 0;
}

static inline void tloc__mark_block_as_used(tloc_allocator *allocator, tloc_header *block) {
	block->size &= ~tloc__BLOCK_IS_FREE;
	if (!tloc__is_last_block(allocator, block)) {
		tloc_header *next_block = tloc__next_physical_block(block);
		next_block->size &= ~tloc__PREV_BLOCK_IS_FREE;
	}
}

static inline void tloc__mark_block_as_free(tloc_allocator *allocator, tloc_header *block) {
	block->size |= tloc__BLOCK_IS_FREE;
	if (!tloc__is_last_block(allocator, block)) {
		tloc_header *next_block = tloc__next_physical_block(block);
		next_block->size |= tloc__PREV_BLOCK_IS_FREE;
	}
}

static inline tloc_bool tloc__valid_allocation_pointer(tloc_allocator *allocator, tloc_header *block) {
	void *block_address = (void*)block;
	return block >= allocator->first_block && block_address < allocator->end_of_pool && (void*)block->prev_physical_block >= (void*)allocator && (void*)block->prev_physical_block < allocator->end_of_pool && block->size > 0 && block->size <= allocator->total_memory;
}

static inline tloc_bool tloc__valid_block(tloc_allocator *allocator, tloc_header *block) {
	int result = 0;
	if (block->prev_physical_block != tloc__end(allocator)) {
		ptrdiff_t diff = (char*)block - (char*)block->prev_physical_block;
		result += tloc__block_size(block->prev_physical_block) + tloc__BLOCK_POINTER_OFFSET == diff;
	}
	else {
		result += 1;
	}
	if (!tloc__is_last_block(allocator, block)) {
		tloc_header *next_block = tloc__next_physical_block(block);
		ptrdiff_t diff = (char*)next_block - (char*)block;
		result += next_block->prev_physical_block == block && tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET == diff;
	}
	else {
		result += 1;
	}
	if (result != 2) {
		int d = 0;
	}
	return result == 2;
}

/*
	Push a block onto the segregated list of free blocks. Called when tloc_Free is called. Generally blocks are
	merged if possible before this is called
*/
static inline void tloc__push_block(tloc_allocator *allocator, tloc_header *block) {
	tloc_index fi;
	tloc_index si;
	//Get the size class of the block
	tloc__map(tloc__block_size(block), tloc__SECOND_LEVEL_INDEX_LOG2, &fi, &si);
	tloc_header *current_block_in_free_list = allocator->segregated_lists[fi][si];
	//Insert the block into the list by updating the next and prev free blocks of
	//this and the current block in the free list. The current block in the free
	//list may well be the end_block in the allocator so this just means that this
	//block will be added as the first block in this class of free blocks.
	block->next_free_block = current_block_in_free_list;
	block->prev_free_block = &allocator->end_block;
	current_block_in_free_list->prev_free_block = block;

	allocator->segregated_lists[fi][si] = block;
	//Flag the bitmaps to mark that this size class now contains a free block
	allocator->first_level_bitmap |= 1ULL << fi;
	allocator->second_level_bitmaps[fi] |= 1 << si;
	tloc__mark_block_as_free(allocator, block);
}

/*
	Remove a block from the segregated list in the allocator and return it. If there is a next free block in the size class
	then move it down the list, otherwise unflag the bitmaps as necessary. This is only called when we're trying to allocate
	some memory with tloc_Allocate and we've determined that there's a suitable free block in segregated_lists.
*/
static inline tloc_header *tloc__pop_block(tloc_allocator *allocator, tloc_index fli, tloc_index sli) {
	tloc_header *block = allocator->segregated_lists[fli][sli];

	//If the block in the segregated list is actually the end_block then something went very wrong.
	//Somehow the segregated lists had the end block assigned but the first or second level bitmaps
	//did not have the masks assigned
	assert(block != &allocator->end_block);
	if (block->next_free_block != &allocator->end_block) {
		//If there are more free blocks in this size class then shift the next one down and terminate the prev_free_block
		allocator->segregated_lists[fli][sli] = block->next_free_block;
		allocator->segregated_lists[fli][sli]->prev_free_block = tloc__end(allocator);
	}
	else {
		//There's no more free blocks in this size class so flag the second level bitmap for this class to 0.
		allocator->segregated_lists[fli][sli] = tloc__end(allocator);
		allocator->second_level_bitmaps[fli] &= ~(1 << sli);
		if (allocator->second_level_bitmaps[fli] == 0) {
			//And if the second level bitmap is 0 then the corresponding bit in the first lebel can be zero'd too.
			allocator->first_level_bitmap &= ~(1ULL << fli);
		}
	}
	tloc__mark_block_as_used(allocator, block);
	return block;
}

/*
	Remove a block from the segregated list. This is only called when we're merging blocks together. The block is
	just removed from the list and marked as used and then merged with an adjacent block.
*/
static inline void tloc__remove_block_from_segregated_list(tloc_allocator *allocator, tloc_header *block) {
	tloc_index fli, sli;
	//Get the size class
	tloc__map(tloc__block_size(block), tloc__SECOND_LEVEL_INDEX_LOG2, &fli, &sli);
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
	tloc__mark_block_as_used(allocator, block);
}

static inline tloc_index tloc__find_next_size_up(tloc_fl_bitmap map, tloc_uint start) {
	//Mask out all bits up to the start point of the scan
	map &= (~0ULL << (start + 1));
	return tloc__scan_forward(map);
}

/*

1)	First check if the size plus the block overhead plus the minimum block size is greater than the block size that we're splitting.
	If so just return the block and don't split.

	This is how things currently look with the block we're splitting and the next block along
	----------------------------------------------------------
	|	block to split							|	next block
	----------------------------------------------------------

2)  If it can be split then get the pointer from the block user pointer (block address + tloc__BLOCK_POINTER_OFFSET). This is the split point.
3)	This gives us the addres of trimmed which is the off cut from the block we don't need that can be added to the segregated list as 
	a free block.

	After the split
	----------------------------------------------------------
	|	block					|	Trimmed		|	next block
	----------------------------------------------------------
			^	Prev block of next still points to block being split
			|_______________________________________|
	
4)	Set the size of trimmed to the left over size after the split.
5)	If the block we're splitting is not the last block in the pool then we need to set the previous physical block pointer in the next
	physical block along to the new trimmed block. And then set the previous physical pointer of the trimmed block to the new block we made after splitting

	After previous physical block pointers are updated
	----------------------------------------------------------
	|	block					|	Trimmed		|	next block
	----------------------------------------------------------
			^						|	^			 |
			|_______________________|	|____________|

6)	Push the trimmed block into the segregated list of free blocks.
7)	Update the block size to what's being requested and pass the user pointer of the block back to the calling function.

*/
static inline void *tloc__maybe_split_block(tloc_allocator *allocator, tloc_header *block, tloc_size size) {
	tloc_size size_plus_overhead = size + tloc__BLOCK_POINTER_OFFSET;
	tloc_header *prev_block = block->prev_physical_block;
	if (size_plus_overhead + tloc__MINIMUM_BLOCK_SIZE > tloc__block_size(block)) {
		return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
	}
	tloc_header *trimmed = (tloc_header*)((char*)tloc__block_user_ptr(block) + size);
	trimmed->size = 0;
	tloc__set_block_size(trimmed, tloc__block_size(block) - size_plus_overhead);
	if (!tloc__is_last_block(allocator, block)) {
		tloc_header *next_block = tloc__next_physical_block(block);
		tloc__set_prev_physical_block(next_block, trimmed);
	}
	tloc__set_prev_physical_block(trimmed, block);
	tloc__set_block_size(block, size);
	tloc__push_block(allocator, trimmed);
	return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
}

/*
	Before merging, blocks look like this:

	|	prev block	|	block being freed		|	next block	(if we're not at the end of the pool)

	Remove the prev block from the segregated list of free blocks. This function will not be called if the
	previous block is not free.
	Add on the size of the current block to the prev block plus the tloc__BLOCK_POINTER_OFFSET
	to reclaim the header overhead

	Now the blocks look like this:
	|	prev and block being freed				|	next block

	Set the prev_physical_block pointer in the next block to point to the new merged block.
*/
static inline tloc_header *tloc__merge_with_prev_block(tloc_allocator *allocator, tloc_header *block) {
	tloc_header *prev_block = block->prev_physical_block;
	tloc_size offset_size = tloc__BLOCK_POINTER_OFFSET + tloc__block_size(prev_block) + tloc__block_size(block);
	tloc_header *next_block = tloc__next_physical_block(block);
	tloc__remove_block_from_segregated_list(allocator, prev_block);
	tloc__set_block_size(prev_block, tloc__block_size(prev_block) + tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET);
	tloc__set_prev_physical_block(next_block, prev_block);
	tloc__zero_block(block);
	return prev_block;
}

/*
	Before the merge blocks look like this: (note this is not called if we're at the end of the pool and there is no next block)

	| prev block	| block being freed 		| next block	|	block after next ---->

	Check that the next block is actually free to be merged
	Remove the next block from the segregated list of free blocks.
	Add on the size of the current block to the next block plus the tloc__BLOCK_POINTER_OFFSET
	to reclaim the header overhead

	Now the blocks look like this:
	| prev block	| next block and block being freed			|	block after next ---->

	Set the prev_physical_block pointer in the block after next to point to the new merged block.
*/
static inline void tloc__merge_with_next_block_if_free(tloc_allocator *allocator, tloc_header *block) {
	tloc_header *next_block = tloc__next_physical_block(block);
	if (next_block->prev_physical_block == block && tloc__is_free_block(next_block)) {
		tloc__remove_block_from_segregated_list(allocator, next_block);
		tloc__set_block_size(block, tloc__block_size(next_block) + tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET);
		if (!tloc__is_last_block(allocator, next_block)) {
			tloc_header *block_after_next = tloc__next_physical_block(next_block);
			tloc__set_prev_physical_block(block_after_next, block);
		}
		tloc__zero_block(next_block);
	}
}
//--End of internal functions

//--End of header declarations

#ifdef __cplusplus
}
#endif

#endif

//Implementation
#if defined(TLOC_IMPLEMENTATION)

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

//Definitions
tloc_allocator *tloc_InitialiseAllocator(void *memory, tloc_size size) {
	if (!memory) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: The memory pointer passed in to the initialiser was NULL, did it allocate properly?\n", TLOC_ERROR_NAME);
		return 0;
	}
	if (!tloc__is_aligned(size, tloc__MEMORY_ALIGNMENT)) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: memory passed to allocator is not aligned to: %u bytes\n", TLOC_ERROR_NAME, tloc__MEMORY_ALIGNMENT);
		return 0;
	}
	tloc_allocator *allocator = (tloc_allocator*)memory;
	memset(allocator, 0, sizeof(tloc_allocator));
	allocator->end_block.next_free_block = &allocator->end_block;
	allocator->end_block.prev_free_block = &allocator->end_block;
	size_t array_offset = sizeof(tloc_allocator);

	tloc_index scan_result;
	//Get the number of first level size categorisations. Must not be larger then 31
	scan_result = tloc__scan_reverse(size);
	tloc_uint first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	//Each first level size class then splits into finer classes by the second level index log2
	tloc_uint second_level_index_count = 1 << tloc__SECOND_LEVEL_INDEX_LOG2;

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
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Tried to initialise allocator with a memory pool that is too small. Must be at least: %zi bytes\n", TLOC_ERROR_NAME, minimum_size);
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
	block->size = 0;
	tloc__set_block_size(block, size - (array_offset + lists_size + tloc__POINTER_SIZE + tloc__BLOCK_SIZE_OVERHEAD));

	//The size of the allocator + initial free memory should add up to the size of memory being used
	assert(tloc__block_size(block) + lists_size + array_offset + tloc__POINTER_SIZE + tloc__BLOCK_SIZE_OVERHEAD == size);
	//Make sure it aligns to nearest multiple of 4
	tloc__set_block_size(block, tloc__align_size_down(tloc__block_size(block), tloc__MEMORY_ALIGNMENT));
	allocator->end_of_pool = tloc__next_physical_block(block);
	assert(tloc__block_size(block) > tloc__MINIMUM_BLOCK_SIZE);
	block->size |= tloc__BLOCK_IS_FREE;
	block->prev_physical_block = &allocator->end_block;
	tloc__push_block(allocator, block);

	assert(tloc__is_last_block(allocator, block));

	block->prev_physical_block = tloc__end(allocator);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);

	return allocator;
}

tloc_size tloc_CalculateAllocationSize(tloc_size size) {
	tloc_size array_offset = sizeof(tloc_allocator);
	tloc_index scan_result;
	scan_result = tloc__scan_reverse(size);
	tloc_uint first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	tloc_uint second_level_index_count = 1 << tloc__SECOND_LEVEL_INDEX_LOG2;

	tloc_uint size_of_second_level_bitmap_list = first_level_index_count * sizeof(tloc_index);
	tloc_uint segregated_list_size = (second_level_index_count * tloc__POINTER_SIZE * first_level_index_count) + (first_level_index_count * tloc__POINTER_SIZE);
	tloc_size lists_size = segregated_list_size + size_of_second_level_bitmap_list;

	return tloc__align_size_up(array_offset + lists_size + size, tloc__MEMORY_ALIGNMENT);
}

tloc_size tloc_AllocatorPoolSize(tloc_allocator *allocator) {
	ptrdiff_t diff = (char*)allocator->end_of_pool - (char*)allocator->first_block;
	return diff;
}

void *tloc_Allocate(tloc_allocator *allocator, tloc_size size) {
	tloc_index fli;
	tloc_index sli;
	size = tloc__align_size_up(size, tloc__MEMORY_ALIGNMENT);
	if (size < tloc__MINIMUM_BLOCK_SIZE) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Trying to allocate a block size that is too small. Minimum size is %u but trying to allocate %zu bytes\n", TLOC_ERROR_NAME, tloc__MINIMUM_BLOCK_SIZE, size);
		return NULL;
	}
	tloc__map(size, tloc__SECOND_LEVEL_INDEX_LOG2, &fli, &sli);
	//Note that there may well be an appropriate size block in the class but that block may not be at the head of the list
	//In this situation we could opt to loop through the list of the size class to size if there is an appropriate size but instead
	//we stick to the paper and just move on to the next class this up to keep a O1 speed at the cost of fragmentation
	if (tloc__has_free_block(allocator, fli, sli) && tloc__block_size(allocator->segregated_lists[fli][sli]) >= size) {
		return tloc__block_user_ptr(tloc__pop_block(allocator, fli, sli));
	}
	if (sli == (1 << tloc__SECOND_LEVEL_INDEX_LOG2) - 1) {
		sli = -1;
	}
	else {
		sli = tloc__find_next_size_up(allocator->second_level_bitmaps[fli], sli);
	}
	if (sli == -1) {
		fli = tloc__find_next_size_up(allocator->first_level_bitmap, fli);
		if (fli > -1) {
			sli = tloc__scan_forward(allocator->second_level_bitmaps[fli]);
			tloc_header *block = tloc__pop_block(allocator, fli, sli);
			assert(tloc_ConfirmBlockLink(allocator, block));
			assert(tloc__block_size(block) > size);
			void *allocation = tloc__maybe_split_block(allocator, block, size);
			return allocation;
		}
	}
	else {
		tloc_header *block = tloc__pop_block(allocator, fli, sli);
		assert(tloc_ConfirmBlockLink(allocator, block));
		assert(tloc__block_size(block) > size);
		void *allocation = tloc__maybe_split_block(allocator, block, size);
		return allocation;
	}
	//Out of memory;
	TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Not enough memory in pool to allocate %zu bytes\n", TLOC_ERROR_NAME, size);
	return NULL;
}

int tloc_Free(tloc_allocator *allocator, void* allocation) {
	tloc_header *block = tloc__block_from_allocation(allocation);
	if (!tloc__valid_allocation_pointer(allocator, block)) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Tried to free an invalid allocation. Pointer doesn't point to a valid block in the allocator or potentially writes were made outside of the allocation bounds.\n", TLOC_ERROR_NAME);
		return 0;
	}
	if (!tloc__valid_block(allocator, block)) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Memory corruption detected. Block you are trying to free no longer links with other blocks correctly suggesting that writes were made outside of the allocation bounds.\n", TLOC_ERROR_NAME);
		return 0;
	}
	if (tloc__prev_is_free_block(block)) {
		assert(block->prev_physical_block);		//Must be a valid previous physical block
		block = tloc__merge_with_prev_block(allocator, block);
	}
	if (!tloc__is_last_block(allocator, block)) {
		tloc__merge_with_next_block_if_free(allocator, block);
	}
	tloc__push_block(allocator, block);
	return 1;
}

int tloc_BlockCount(tloc_allocator *allocator) {
	tloc_header *current_block = allocator->first_block;
	int count = 1;
	while (!tloc__is_last_block(allocator, current_block)) {
		count++;
		current_block = tloc__next_physical_block(current_block);
	}
	return count;
}

//--- Debugging tools
tloc_bool tloc_ValidPointer(tloc_allocator *allocator, void *pointer) {
	return pointer >= (void*)allocator && pointer <= (void*)((char*)allocator + allocator->total_memory);
}

tloc_bool tloc_ValidBlock(tloc_allocator *allocator, tloc_header *block) {
	if (block == &allocator->end_block || tloc__is_last_block(allocator, block)) {
		return 1;
	}
	if (tloc_ValidPointer(allocator, (void*)block) && tloc_ValidPointer(allocator, (char*)block + sizeof(tloc_header))) {
		tloc_bool valid_prev_block = tloc_ValidPointer(allocator, (void*)block->prev_physical_block);
		tloc_bool valid_next_block = tloc_ValidPointer(allocator, (char*)tloc__next_physical_block(block) + sizeof(tloc_header));
		return valid_prev_block && valid_next_block;
	}
	return 0;
}

tloc_bool tloc_ConfirmBlockLink(tloc_allocator *allocator, tloc_header *block) {
	int result = 0;
	if (block->prev_physical_block != tloc__end(allocator)) {
		ptrdiff_t diff = (char*)block - (char*)block->prev_physical_block;
		result += tloc__block_size(block->prev_physical_block) + tloc__BLOCK_POINTER_OFFSET == diff;
	}
	else {
		result += 1;
	}
	if (!tloc__is_last_block(allocator, block)) {
		tloc_header *next_block = tloc__next_physical_block(block);
		ptrdiff_t diff = (char*)next_block - (char*)block;
		result += tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET == diff;
	}
	else {
		result += 1;
	}
	if (result != 2) {
		int d = 0;
	}
	return result == 2;
}

static void tloc__output(void* ptr, size_t size, int free, void* user, int is_final_output)
{
	(void)user;
	printf("\t%p %s size: %zi (%p)\n", ptr, free ? "free" : "used", size, ptr);
	if (is_final_output) {
		printf("\t------------- * ---------------\n");
	}
}

tloc_header *tloc_SearchList(tloc_allocator *allocator, tloc_header *search) {
	tloc_header *current_block = allocator->first_block;
	if (search == current_block) {
		return current_block;
	}
	if (!tloc_ValidPointer(allocator, (void*)current_block)) {
		return 0;
	}
	while (!tloc__is_last_block(allocator, current_block)) {
		if (search == current_block) {
			return current_block;
		}
		current_block = tloc__next_physical_block(current_block);
	}
	return current_block == search ? current_block : 0;
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
		tloc_header *prev_block = current_block->prev_physical_block;
		if (prev_block != tloc__end(allocator)) {
			ptrdiff_t diff = (char*)current_block - (char*)prev_block;
			tloc_size expected_diff = tloc__block_size(prev_block) + tloc__BLOCK_POINTER_OFFSET;
			if (diff != tloc__block_size(prev_block) + tloc__BLOCK_POINTER_OFFSET) {
				return tloc__PHYSICAL_BLOCK_MISALIGNMENT;
			}
		}
		tloc_header *last_block = current_block;
		current_block = tloc__next_physical_block(current_block);
		if (last_block != current_block->prev_physical_block) {
			return tloc__PHYSICAL_BLOCK_MISALIGNMENT;
		}
		if (!tloc_ConfirmBlockLink(allocator, current_block)) {
			return tloc__PHYSICAL_BLOCK_MISALIGNMENT;
		}
	}
	if (output_function) {
		tloc__output(current_block, tloc__block_size(current_block), tloc__is_free_block(current_block), user_data, 1);
	}
	return tloc__OK;
}

tloc_bool tloc_CheckForNullBlocksInList(tloc_allocator *allocator) {
	tloc_index scan_result;
	scan_result = tloc__scan_reverse(allocator->total_memory);
	tloc_index first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	tloc_index second_level_index_count = 1 << tloc__SECOND_LEVEL_INDEX_LOG2;
	for (int fli = 0; fli != first_level_index_count; ++fli) {
		if (allocator->first_level_bitmap & (1ULL << fli)) {
			for (int sli = 0; sli != second_level_index_count; ++sli) {
				if (allocator->second_level_bitmaps[fli] & (1U << sli)) {
					if (allocator->segregated_lists[fli][sli]->prev_physical_block == 0) {
						return 0;
					}
				}
			}
		}
	}
	return 1;
}

tloc_bool tloc_BlockExistsInSegregatedList(tloc_allocator *allocator, tloc_header* block) {
	tloc_index scan_result;
	scan_result = tloc__scan_reverse(allocator->total_memory);
	tloc_index first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	tloc_index second_level_index_count = 1 << tloc__SECOND_LEVEL_INDEX_LOG2;
	for (int fli = 0; fli != first_level_index_count; ++fli) {
		for (int sli = 0; sli != second_level_index_count; ++sli) {
			tloc_header *current = allocator->segregated_lists[fli][sli];
			while (current != tloc__end(allocator)) {
				if (current == block) {
					return 1;
				}
				current = current->next_free_block;
			}
		}
	}
	return 0;
}

tloc__error_codes tloc_VerifySegregatedLists(tloc_allocator *allocator) {
	tloc_index scan_result;
	scan_result = tloc__scan_reverse(allocator->total_memory);
	tloc_index first_level_index_count = tloc__Min(scan_result, tloc__FIRST_LEVEL_INDEX_MAX) + 1;
	tloc_index second_level_index_count = 1 << tloc__SECOND_LEVEL_INDEX_LOG2;
	for (int fli = 0; fli != first_level_index_count; ++fli) {
		for (int sli = 0; sli != second_level_index_count; ++sli) {
			tloc_header *block = allocator->segregated_lists[fli][sli];
			if (block->size) {
				tloc_index size_fli, size_sli;
				tloc__map(tloc__block_size(block), tloc__SECOND_LEVEL_INDEX_LOG2, &size_fli, &size_sli);
				if (size_fli != fli && size_sli != sli) {
					return tloc__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST;
				}
			}
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
