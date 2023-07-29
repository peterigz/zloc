
/*	2Loc, a Two Level Segregated Fit memory allocator

	This software is dual-licensed to the public domain and under the following
	license: you are granted a perpetual, irrevocable license to copy, modify,
	publish, and distribute this file as you see fit.

	This library is based on the following paper:

	TLSF: a New Dynamic Memory Allocator for Real-Time Systems [not so new now, the paper was from 2005]
	M. Masmano, I. Ripoll, A. Crespo, and J. Real Universidad Politecnica de Valencia, Spain
	http://www.gii.upv.es/tlsf/files/ecrts04_tlsf.pdf

	Thanks to the authors of the paper and also Sean Barret for his how to make a single header-file
	library guidelines, and also to Matthew Conte who's own TLSF lib I referenced when trying to understand how
	the algorythm works. His library can be found here: https://github.com/mattconte/tlsf

	What's this library for?
	This library is for sub allocating memory blocks within a larger memory allocation that you might
	create with malloc or VirtualAlloc etc. 
	
	Allocation and freeing those memory blocks happens at O(1) time complexity and should for the most 
	part keep fragmentation at a minimum.

	How do I use it?
	Add:
	#define TLOC_IMPLEMENTATION
	before you include this file in *one* C or C++ file to create the implementation.

   // i.e. it should look like this:
	#include ...
	#include ...
	#include ...
	#define TLOC_IMPLEMENTATION
	#include "2loc.h"

	The interface is very straightforward. Simply allocate a block of memory that you want to use for your
	pool and then call tloc_Allocate to allocate blocks within that pool and tloc_Free when you're done with
	an allocation. Don't forget to free the orinal memory you created in the first place. The Allocator doesn't
	care what you use to create the memory to use with the allocator only that it's read and writable to.

	Here's a basic usage example:

	tloc_size size = 1024 * 1024 * 128;	//128MB
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
	TLOC_ASSERT(allocator); Something went wrong, unable to initialise the allocator

	int *int_allocation = tloc_Allocate(allocator, sizeof(int) * 100);
	if(int_allocation) {
		for (int i = 0; i != 100; ++i) {
			int_allocation[i] = rand();
		}
		for (int i = 0; i != 100; ++i) {
			printf("%i\n", int_allocation[i]);
		}
		TLOC_ASSERT(tloc_Free(allocator, int_allocation));	//Unable to free the allocation
	} else {
		//Unable to allocate
	}
	free(memory);

	You can also take a look at the tests.c file for more examples of usage.

	Is it thread safe?
	define TLOC_THREAD_SAFE before you include 2loc.h to make each call to tloc_Allocate and tloc_Free to add 
	basic thread safety. Basically all it does is lock the allocator so that only one process can free or 
	allocate at the same time. Future versions would probably handle this with separate pools per thread.

	Other options:
	Define TLOC_OUTPUT_ERROR_MESSAGES to switch on logging errors to the console for some more feedback on errors
	like out of memory or corrupted block detection.
*/

#ifndef TLOC_INCLUDE_H
#define TLOC_INCLUDE_H

//#define TLOC_DEV_MODE

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>

//Header
#define tloc__Min(a, b) (((a) < (b)) ? (a) : (b))
#define tloc__Max(a, b) (((a) > (b)) ? (a) : (b))

#ifndef TLOC_API
#define TLOC_API
#endif

typedef int tloc_index;
typedef unsigned int tloc_sl_bitmap;
typedef unsigned int tloc_uint;
typedef unsigned int tloc_thread_access;
typedef int tloc_bool;
typedef void* tloc_pool;

#if !defined (TLOC_ASSERT)
#define TLOC_ASSERT assert
#endif

#define tloc__is_pow2(x) ((x) && !((x) & ((x) - 1)))
#define tloc__glue2(x, y) x ## y
#define tloc__glue(x, y) tloc__glue2(x, y)
#define tloc__static_assert(exp) \
	typedef char tloc__glue(static_assert, __LINE__) [(exp) ? 1 : -1]

#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__x86_64__)
#define tloc__64BIT
typedef size_t tloc_size;
typedef size_t tloc_fl_bitmap;
#define TLOC_ONE 1ULL
#else
typedef size_t tloc_size;
typedef size_t tloc_fl_bitmap;
#define TLOC_ONE 1U
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

#ifndef TLOC_MAX_SIZE_INDEX
#if defined(tloc__64BIT)
#define TLOC_MAX_SIZE_INDEX 32
#else
#define TLOC_MAX_SIZE_INDEX 30
#endif
#endif

tloc__static_assert(TLOC_MAX_SIZE_INDEX < 64);

#ifdef __cplusplus
extern "C" {
#endif

enum tloc__constants {
	tloc__MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
	tloc__MINIMUM_BLOCK_SIZE = 16,
	tloc__MINIMUM_POOL_SIZE = tloc__MEGABYTE(1),
	tloc__SECOND_LEVEL_INDEX_LOG2 = 5,
	tloc__FIRST_LEVEL_INDEX_COUNT = TLOC_MAX_SIZE_INDEX,
	tloc__SECOND_LEVEL_INDEX_COUNT = 1 << tloc__SECOND_LEVEL_INDEX_LOG2,
	tloc__FIRST_LEVEL_INDEX_MAX = (1 << (MEMORY_ALIGNMENT_LOG2 + 3)) - 1,
	tloc__BLOCK_POINTER_OFFSET = sizeof(void*) + sizeof(tloc_size),
	tloc__BLOCK_SIZE_OVERHEAD = sizeof(tloc_size),
	tloc__POINTER_SIZE = sizeof(void*)
};

typedef enum tloc__boundary_tag_flags {
	tloc__BLOCK_IS_FREE = 1 << 0,
	tloc__PREV_BLOCK_IS_FREE = 1 << 1,
} tloc__boundary_tag_flags;

typedef enum tloc__error_codes {
	tloc__OK,
	tloc__INVALID_FIRST_BLOCK,
	tloc__INVALID_BLOCK_FOUND,
	tloc__PHYSICAL_BLOCK_MISALIGNMENT,
	tloc__INVALID_SEGRATED_LIST,
	tloc__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST,
	tloc__SECOND_LEVEL_BITMAPS_NOT_INITIALISED
} tloc__error_codes;

typedef enum tloc__thread_ops {
	tloc__FREEING_BLOCK = 1 << 0,
	tloc__ALLOCATING_BLOCK = 1 << 1
} tloc__thread_ops;

/*
	Each block has a header that if used only has a pointer to the previous physical block
	and the size. If the block is free then the prev and next free blocks are also stored.
*/
typedef struct tloc_header {
	struct tloc_header *prev_physical_block;
	/*	Note that the size is either 4 or 8 bytes aligned so the boundary tag (2 flags denoting
		whether this or the previous block is free) can be stored in the first 2 least
		significant bits	*/
	tloc_size size;
	struct tloc_header *prev_free_block;
	struct tloc_header *next_free_block;
} tloc_header;

typedef struct tloc_allocator {
	/*	This is basically a terminator block that blocks can point to if they're at the end
		of a list. A list could be blocks in a size class of the segregate list or the 
		physical chain of blocks.	*/
	tloc_header end_block;
#if defined(TLOC_THREAD_SAFE)
	/* Multithreading protection*/
	volatile tloc_thread_access access;
#endif
	/*	Here we store all of the free block data. first_level_bitmap is either a 32bit int
	or 64bit depending on the mode you're in. second_level_bitmaps are an array of 32bit
	ints. segregated_lists is a two level array pointing to free blocks. */
	tloc_fl_bitmap first_level_bitmap;
	tloc_sl_bitmap second_level_bitmaps[tloc__FIRST_LEVEL_INDEX_COUNT];
	tloc_header *segregated_lists[tloc__FIRST_LEVEL_INDEX_COUNT][tloc__SECOND_LEVEL_INDEX_COUNT];
} tloc_allocator;

#if defined (_MSC_VER) && (_MSC_VER >= 1400) && (defined (_M_IX86) || defined (_M_X64))
/* Microsoft Visual C++ support on x86/X64 architectures. */

#include <intrin.h>

static inline int tloc__scan_reverse(tloc_size bitmap) {
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

#ifdef _WIN32
#include <Windows.h>
static inline tloc_thread_access tloc__compare_and_exchange(volatile tloc_thread_access* target, tloc_thread_access value, tloc_thread_access original) {
	return InterlockedCompareExchange(target, value, original);
}
#endif

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

static inline tloc_thread_access tloc__compare_and_exchange(volatile tloc_thread_access* target, tloc_thread_access value, tloc_thread_access original) {
	return __sync_val_compare_and_swap(target, original, value);
}

#endif

/*
User functions
This are the main functions you'll need to use this library, everything else is either internal private functions or functions for debugging
*/

/*
	Initialise an allocator. Pass a block of memory that you want to use to store the allocator data. This will not create a pool, only the 
	necessary data structures to store the allocator.

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
	Initialise an allocator and a pool at the same time. The data stucture to store the allocator will be stored at the beginning of the memory
	you pass to the function and the remaining memory will be used as the pool.

	@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
	@param	tloc_size				The size of the memory you're passing
	@returns tloc_allocator*		A pointer to a tloc_allocator which you'll need to use when calling tloc_Allocate or tloc_Free.
									If something went wrong then 0 is returned. Define TLOC_OUTPUT_ERROR_MESSAGES before including this header
									file to see any errors in the console.
*/
TLOC_API tloc_allocator *tloc_InitialiseAllocatorWithPool(void *memory, tloc_size size);

/*
	Add a new memory pool to the allocator. Pools don't have to all be the same size, adding a pool will create the biggest block it can within
	the pool and then add that to the segregated list of free blocks in the allocator. All the pools in the allocator will be naturally linked
	together in the segregated list because all blocks are linked together with a linked list either as physical neighbours or free blocks in
	the segregated list.

	@param	tloc_allocator*			A pointer to some previously initialised allocator
	@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
	@param	tloc_size				The size of the memory you're passing
	@returns tloc_pool*				A pointer to the pool 
*/
TLOC_API tloc_pool *tloc_AddPool(tloc_allocator *allocator, void *memory, tloc_size size);

/*
	Get the structure size of an allocator. You can use this to take into account the overhead of the allocator when preparing a new allocator
	with memory pool.

	@returns tloc_size				The struct size of the allocator in bytes
*/
tloc_size tloc_AllocatorSize();

/*
	If you initialised an allocator with a pool then you can use this function to get a pointer to the start of the pool. It won't get a pointer
	to any other pool in the allocator. You can just get that when you call tloc_AddPool.

	@param	tloc_allocator*			A pointer to some previously initialised allocator
	@returns tloc_pool				A pointer to the pool memory in the allocator
*/
TLOC_API tloc_pool *tloc_GetPool(tloc_allocator *allocator);

/*
	Allocate some memory within a tloc_allocator of the specified size. Minimum size is 16 bytes. 

	@param	tloc_size				The size of the memory you're passing
	@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to 
									no free memory. If that happens then you may want to add a pool at that point.
*/
TLOC_API void *tloc_Allocate(tloc_allocator *allocator, tloc_size size);

/*
	Free an allocation from a tloc_allocator. When freeing a block of memory any adjacent free blocks are merged together to keep on top of 
	fragmentation as much as possible. A check is also done to confirm that the block being freed is still valid and detect any memory corruption
	due to out of bounds writing of this or potentially other blocks.  

	It's recommended to call this function with an assert: TLOC_ASSERT(tloc_Free(allocator, allocation));
	An error is also output to console as long as TLOC_OUTPUT_ERROR_MESSAGES is defined.

	@returns int		returns 1 if the allocation was successfully freed, 0 otherwise.
*/
TLOC_API int tloc_Free(tloc_allocator *allocator, void *allocation);

/*
	Remove a pool from an allocator. Note that all blocks in the pool must be free and therefore all merged together into one block (this happens
	automatically as all blocks are freed are merged together into bigger blocks.

	@param tloc_allocator*			A pointer to a tcoc_allocator that you want to reset
	@param tloc_allocator*			A pointer to the memory pool that you want to free. You get this pointer when you add a pool to the allocator.
*/
TLOC_API tloc_bool tloc_RemovePool(tloc_allocator *allocator, tloc_pool *pool);

//--End of user functions

//Private inline functions, user doesn't need to call these

static inline void tloc__map(tloc_size size, tloc_index *fli, tloc_index *sli) {
	*fli = tloc__scan_reverse(size);
	size = size & ~(1 << *fli);
	*sli = (tloc_index)(size >> (*fli - tloc__SECOND_LEVEL_INDEX_LOG2)) % tloc__SECOND_LEVEL_INDEX_COUNT;
}

//Read only functions
static inline tloc_bool tloc__has_free_block(const tloc_allocator *allocator, tloc_index fli, tloc_index sli) {
	return allocator->first_level_bitmap & (TLOC_ONE << fli) && allocator->second_level_bitmaps[fli] & (1U << sli);
}

static inline tloc_bool tloc__is_used_block(const tloc_header *block) {
	return !(block->size & tloc__BLOCK_IS_FREE);
}

static inline tloc_bool tloc__is_free_block(const tloc_header *block) {
	return block->size & tloc__BLOCK_IS_FREE;
}

static inline tloc_bool tloc__prev_is_free_block(const tloc_header *block) {
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

static inline tloc_size tloc__block_size(const tloc_header *block) {
	return block->size & ~(tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE);;
}

static inline tloc_header *tloc__block_from_allocation(const void *allocation) {
	return (tloc_header*)((char*)allocation - tloc__BLOCK_POINTER_OFFSET);
}

static inline tloc_header *tloc__end(tloc_allocator *allocator) {
	return &allocator->end_block;
}

static inline void* tloc__block_user_ptr(const tloc_header *block) {
	return (char*)block + tloc__BLOCK_POINTER_OFFSET;
}

static inline tloc_header* tloc__first_block_in_pool(const tloc_pool *pool) {
	return (tloc_header*)((char*)pool - tloc__POINTER_SIZE);
}

static inline tloc_header *tloc__next_physical_block(const tloc_header *block) {
	return (tloc_header*)((char*)tloc__block_user_ptr(block) + tloc__block_size(block));
}

static inline tloc_bool tloc__next_block_is_free(const tloc_header *block) {
	return tloc__is_free_block(tloc__next_physical_block(block));
}

static inline tloc_header *tloc__allocator_first_block(tloc_allocator *allocator) {
	return (tloc_header*)((char*)allocator + tloc_AllocatorSize() - tloc__POINTER_SIZE);
}

static inline tloc_bool tloc__is_last_block_in_pool(const tloc_header *block) {
	return tloc__block_size(block) == 0;
}

static inline tloc_index tloc__find_next_size_up(tloc_fl_bitmap map, tloc_uint start) {
	//Mask out all bits up to the start point of the scan
	map &= (~0ULL << (start + 1));
	return tloc__scan_forward(map);
}

//Write functions
#if defined(TLOC_THREAD_SAFE)

static inline void tloc__lock_thread_access(tloc_allocator *allocator) {
	do {
	} while (0 != tloc__compare_and_exchange(&allocator->access, 1, 0));
}

static inline void tloc__unlock_thread_access(tloc_allocator *allocator) {
	allocator->access = 0;
}

#endif

static inline void tloc__set_block_size(tloc_header *block, tloc_size size) {
	tloc_size boundary_tag = block->size & (tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE);
	block->size = size | boundary_tag;
}

static inline void tloc__set_prev_physical_block(tloc_header *block, tloc_header *prev_block) {
	block->prev_physical_block = prev_block;
}

static inline void tloc__zero_block(tloc_header *block) {
	block->prev_physical_block = 0;
	block->size = 0;
}

static inline void tloc__mark_block_as_used(tloc_allocator *allocator, tloc_header *block) {
	block->size &= ~tloc__BLOCK_IS_FREE;
	tloc_header *next_block = tloc__next_physical_block(block);
	next_block->size &= ~tloc__PREV_BLOCK_IS_FREE;
}

static inline void tloc__mark_block_as_free(tloc_header *block) {
	block->size |= tloc__BLOCK_IS_FREE;
	tloc_header *next_block = tloc__next_physical_block(block);
	next_block->size |= tloc__PREV_BLOCK_IS_FREE;
}

static inline void tloc__block_set_used(tloc_header *block) {
	block->size &= ~tloc__BLOCK_IS_FREE;
}

static inline void tloc__block_set_free(tloc_header *block) {
	block->size |= tloc__BLOCK_IS_FREE;
}

static inline void tloc__block_set_prev_used(tloc_header *block) {
	block->size &= ~tloc__PREV_BLOCK_IS_FREE;
}

static inline void tloc__block_set_prev_free(tloc_header *block) {
	block->size |= tloc__PREV_BLOCK_IS_FREE;
}

/*
	Push a block onto the segregated list of free blocks. Called when tloc_Free is called. Generally blocks are
	merged if possible before this is called
*/
static inline void tloc__push_block(tloc_allocator *allocator, tloc_header *block) {
	tloc_index fli;
	tloc_index sli;
	//Get the size class of the block
	tloc__map(tloc__block_size(block), &fli, &sli);
	tloc_header *current_block_in_free_list = allocator->segregated_lists[fli][sli];
	//Insert the block into the list by updating the next and prev free blocks of
	//this and the current block in the free list. The current block in the free
	//list may well be the end_block in the allocator so this just means that this
	//block will be added as the first block in this class of free blocks.
	block->next_free_block = current_block_in_free_list;
	block->prev_free_block = &allocator->end_block;
	current_block_in_free_list->prev_free_block = block;

	allocator->segregated_lists[fli][sli] = block;
	//Flag the bitmaps to mark that this size class now contains a free block
	allocator->first_level_bitmap |= TLOC_ONE << fli;
	allocator->second_level_bitmaps[fli] |= 1 << sli;
	tloc__mark_block_as_free(block);
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
	TLOC_ASSERT(block != &allocator->end_block);
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
			allocator->first_level_bitmap &= ~(TLOC_ONE << fli);
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
	tloc__map(tloc__block_size(block), &fli, &sli);
	tloc_header *prev_block = block->prev_free_block;
	tloc_header *next_block = block->next_free_block;
	TLOC_ASSERT(prev_block);
	TLOC_ASSERT(next_block);
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

/*
	This function is called when tloc_Allocate is called. Once a free block is found then it will be split
	if the size + header overhead + the minimum block size (16b) is greater then the size of the free block.
	If not then it simply returns the free block as it is without splitting. 
	If split then the trimmed amount is added back to the segregated list of free blocks.
*/
static inline void *tloc__maybe_split_block(tloc_allocator *allocator, tloc_header *block, tloc_size size) {
	TLOC_ASSERT(!tloc__is_last_block_in_pool(block));
	tloc_size size_plus_overhead = size + tloc__BLOCK_POINTER_OFFSET;
	if (size_plus_overhead + tloc__MINIMUM_BLOCK_SIZE > tloc__block_size(block)) {
		return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
	}
	tloc_header *trimmed = (tloc_header*)((char*)tloc__block_user_ptr(block) + size);
	trimmed->size = 0;
	tloc__set_block_size(trimmed, tloc__block_size(block) - size_plus_overhead);
	tloc_header *next_block = tloc__next_physical_block(block);
	tloc__set_prev_physical_block(next_block, trimmed);
	tloc__set_prev_physical_block(trimmed, block);
	tloc__set_block_size(block, size);
	tloc__push_block(allocator, trimmed);
	return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
}

/*
	This function is called when tloc_Free is called and the previous physical block is free. If that's the case
	then this function will merge the block being freed with the previous physical block then add that back into 
	the segregated list of free blocks. Note that that happens in the tloc_Free function after attempting to merge
	both ways.
*/
static inline tloc_header *tloc__merge_with_prev_block(tloc_allocator *allocator, tloc_header *block) {
	TLOC_ASSERT(!tloc__is_last_block_in_pool(block));
	tloc_header *prev_block = block->prev_physical_block;
	tloc_size offset_size = tloc__BLOCK_POINTER_OFFSET + tloc__block_size(prev_block) + tloc__block_size(block);
	tloc__remove_block_from_segregated_list(allocator, prev_block);
	tloc__set_block_size(prev_block, tloc__block_size(prev_block) + tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET);
	tloc_header *next_block = tloc__next_physical_block(block);
	tloc__set_prev_physical_block(next_block, prev_block);
	tloc__zero_block(block);
	return prev_block;
}

/*
	This function might be called when tloc_Free is called to free a block. If the block being freed is not the last 
	physical block then this function is called and if the next block is free then it will be merged.
*/
static inline void tloc__merge_with_next_block(tloc_allocator *allocator, tloc_header *block) {
	tloc_header *next_block = tloc__next_physical_block(block);
	TLOC_ASSERT(next_block->prev_physical_block == block);	//could be potentional memory corruption. Check that you're not write outside the boundary of the block size
	TLOC_ASSERT(!tloc__is_last_block_in_pool(next_block));
	tloc__remove_block_from_segregated_list(allocator, next_block);
	tloc__set_block_size(block, tloc__block_size(next_block) + tloc__block_size(block) + tloc__BLOCK_POINTER_OFFSET);
	tloc_header *block_after_next = tloc__next_physical_block(next_block);
	tloc__set_prev_physical_block(block_after_next, block);
	tloc__zero_block(next_block);
}
//--End of internal functions

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

	tloc_size array_offset = sizeof(tloc_allocator);
	if (size < array_offset) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Tried to initialise allocator with a memory allocation that is too small. Must be at least: %zi bytes\n", TLOC_ERROR_NAME, array_offset);
		return 0;
	}

	tloc_allocator *allocator = (tloc_allocator*)memory;
	memset(allocator, 0, sizeof(tloc_allocator));
	allocator->end_block.next_free_block = &allocator->end_block;
	allocator->end_block.prev_free_block = &allocator->end_block;

	//Point all of the segregated list array pointers to the empty block
	for (tloc_uint i = 0; i < tloc__FIRST_LEVEL_INDEX_COUNT; i++) {
		for (tloc_uint j = 0; j < tloc__SECOND_LEVEL_INDEX_COUNT; j++) {
			allocator->segregated_lists[i][j] = &allocator->end_block;
		}
	}

	return allocator;
}

tloc_allocator *tloc_InitialiseAllocatorWithPool(void *memory, tloc_size size) {
	tloc_size array_offset = sizeof(tloc_allocator);
	if (size < array_offset + tloc__MINIMUM_POOL_SIZE) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Tried to initialise allocator with a memory allocation that is too small. Must be at least: %zi bytes\n", TLOC_ERROR_NAME, array_offset + tloc__MINIMUM_POOL_SIZE);
		return 0;
	}

	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
	if (!allocator) {
		return 0;
	}
	tloc_AddPool(allocator, tloc_GetPool(allocator), size - tloc_AllocatorSize());
	return allocator;
}

tloc_size tloc_AllocatorSize() {
	return sizeof(tloc_allocator);
}

tloc_pool *tloc_GetPool(tloc_allocator *allocator) {
	return (void*)((char*)allocator + tloc_AllocatorSize());
}

tloc_pool *tloc_AddPool(tloc_allocator *allocator, void *memory, tloc_size size) {
#if defined(TLOC_THREAD_SAFE)
	tloc__lock_thread_access(allocator);
	TLOC_ASSERT(allocator->access != 0);
#endif
	//Offset it back by the pointer size, we don't need the prev_physical block pointer as there is none
	//for the first block in the pool
	tloc_header *block = tloc__first_block_in_pool(memory);
	block->size = 0;
	//Leave room for an end block
	tloc__set_block_size(block, size - (tloc__BLOCK_POINTER_OFFSET) - tloc__BLOCK_SIZE_OVERHEAD);

	//Make sure it aligns
	tloc__set_block_size(block, tloc__align_size_down(tloc__block_size(block), tloc__MEMORY_ALIGNMENT));
	TLOC_ASSERT(tloc__block_size(block) > tloc__MINIMUM_BLOCK_SIZE);
	tloc__block_set_free(block);
	tloc__block_set_prev_used(block);

	//Add a 0 sized block at the end of the pool to cap it off
	tloc_header *end_block = tloc__next_physical_block(block);
	end_block->size = 0;
	tloc__block_set_used(end_block);

	end_block->prev_physical_block = block;
	tloc__push_block(allocator, block);

#if defined(TLOC_THREAD_SAFE)
	tloc__unlock_thread_access(allocator);
#endif
	return memory;
}

tloc_bool tloc_RemovePool(tloc_allocator *allocator, tloc_pool *pool) {
#if defined(TLOC_THREAD_SAFE)
	tloc__lock_thread_access(allocator);
	TLOC_ASSERT(allocator->access != 0);
#endif
	tloc_header *block = tloc__first_block_in_pool(pool);
	
	if (tloc__is_free_block(block) && !tloc__next_block_is_free(block) && tloc__is_last_block_in_pool(tloc__next_physical_block(block))) {
		tloc__remove_block_from_segregated_list(allocator, block);
#if defined(TLOC_THREAD_SAFE)
		tloc__unlock_thread_access(allocator);
#endif
		return 1;
	}
#if defined(TLOC_THREAD_SAFE)
	tloc__unlock_thread_access(allocator);
	TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: In order to remove a pool there must be only 1 free block in the pool. Was possibly freed by another thread\n", TLOC_ERROR_NAME);
#else
	TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: In order to remove a pool there must be only 1 free block in the pool.\n", TLOC_ERROR_NAME);
#endif
	return 0;
}

void *tloc_Allocate(tloc_allocator *allocator, tloc_size size) {
#if defined(TLOC_THREAD_SAFE)
	tloc__lock_thread_access(allocator);
	TLOC_ASSERT(allocator->access != 0);
#endif
	tloc_index fli;
	tloc_index sli;
	size = tloc__align_size_up(size, tloc__MEMORY_ALIGNMENT);
	if (size < tloc__MINIMUM_BLOCK_SIZE) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Trying to allocate a block size that is too small. Minimum size is %u but trying to allocate %zu bytes\n", TLOC_ERROR_NAME, tloc__MINIMUM_BLOCK_SIZE, size);
		return NULL;
	}
	else if (size > (TLOC_ONE << tloc__FIRST_LEVEL_INDEX_COUNT)) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Trying to allocate a block size that is too large. The largest block size that this allocator allows is %zu bytes but trying to allocate %zu. You can define your own TLOC_MAX_SIZE_INDEX to increase before including 2loc. The max block size is calculated with 1 << TLOC_MAX_SIZE_INDEX.\n", TLOC_ERROR_NAME, (TLOC_ONE << TLOC_MAX_SIZE_INDEX), size);
		return NULL;
	}
	tloc__map(size, &fli, &sli);
	//Note that there may well be an appropriate size block in the class but that block may not be at the head of the list
	//In this situation we could opt to loop through the list of the size class to see if there is an appropriate size but instead
	//we stick to the paper and just move on to the next class up to keep a O1 speed at the cost of some extra fragmentation
	if (tloc__has_free_block(allocator, fli, sli) && tloc__block_size(allocator->segregated_lists[fli][sli]) >= size) {
		void *user_ptr = tloc__block_user_ptr(tloc__pop_block(allocator, fli, sli));
#if defined(TLOC_THREAD_SAFE)
		tloc__unlock_thread_access(allocator);
#endif
		return user_ptr;
	}
	if (sli == tloc__SECOND_LEVEL_INDEX_COUNT - 1) {
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
			TLOC_ASSERT(tloc__block_size(block) > size);
			void *allocation = tloc__maybe_split_block(allocator, block, size);
#if defined(TLOC_THREAD_SAFE)
			tloc__unlock_thread_access(allocator);
#endif
			return allocation;
		}
	}
	else {
		tloc_header *block = tloc__pop_block(allocator, fli, sli);
		TLOC_ASSERT(tloc__block_size(block) > size);
		void *allocation = tloc__maybe_split_block(allocator, block, size);
#if defined(TLOC_THREAD_SAFE)
		tloc__unlock_thread_access(allocator);
#endif
		return allocation;
	}
	//Out of memory;
	TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Not enough memory in pool to allocate %zu bytes\n", TLOC_ERROR_NAME, size);
#if defined(TLOC_THREAD_SAFE)
	tloc__unlock_thread_access(allocator);
#endif
	return 0;
}

int tloc_Free(tloc_allocator *allocator, void* allocation) {
	TLOC_ASSERT(allocation);		//Tried to free a null pointer
#if defined(TLOC_THREAD_SAFE)
	tloc__lock_thread_access(allocator);
	TLOC_ASSERT(allocator->access != 0);
#endif
	tloc_header *block = tloc__block_from_allocation(allocation);
	if (tloc__prev_is_free_block(block)) {
		TLOC_ASSERT(block->prev_physical_block);		//Must be a valid previous physical block
		block = tloc__merge_with_prev_block(allocator, block);
	}
	if (tloc__next_block_is_free(block)) {
		tloc__merge_with_next_block(allocator, block);
	}
	tloc__push_block(allocator, block);
#if defined(TLOC_THREAD_SAFE)
	tloc__unlock_thread_access(allocator);
#endif
	return 1;
}

//--- End Debugging tools
#endif
