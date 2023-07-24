
/*	2Loc, a Two Level Segregated Fit memory allocator

	This software is dual-licensed to the public domain and under the following
	license: you are granted a perpetual, irrevocable license to copy, modify,
	publish, and distribute this file as you see fit.

	This library is based on the following paper:

	TLSF: a New Dynamic Memory Allocator for Real-Time Systems
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

	What's the ideal usage?
	I wrote this for a particle effects library that I'm working on where I want to allocate a large
	block of memory up front and then to efficiently allocate within that block rather then have a whole
	bunch of mallocs getting created and freed all the time. Particle effects generally require a lot of 
	dynamic allocations and it can get messy quick. It would also be nice that anyone that wants to use my
	particle effects library can know precisely how much memory is in use by the library and have an
	easy way of dumping that to disk if they wanted to snapshot the current state of whatever game they're
	working on. That's a big pain to do if you're allocating memory all over the place with something like
	malloc or worse still, new.

	This way they can initialise the libray with a specific amount of memory and know that the library will
	use no more then that and gracefully fail if the library runs out of memory to use.

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

	size_t size = 1024 * 1024 * 128;	//128MB
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
	assert(allocator); Something went wrong, unable to initialise the allocator

	int *int_allocation = tloc_Allocate(allocator, sizeof(int) * 100);
	if(int_allocation) {
		for (int i = 0; i != 100; ++i) {
			int_allocation[i] = rand();
		}
		for (int i = 0; i != 100; ++i) {
			printf("%i\n", int_allocation[i]);
		}
		assert(tloc_Free(allocator, int_allocation));	//Unable to free the allocation
	} else {
		//Unable to allocate
	}
	free(memory);

	You can also take a look at the tests.c file for more examples of usage.

*/

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
typedef unsigned int tloc_sl_bitmap;
typedef unsigned int tloc_uint;
typedef int tloc_bool;

#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__x86_64__)
#ifndef TLOC_FORCE_32BIT
#define tloc__64BIT
typedef size_t tloc_size;
typedef size_t tloc_fl_bitmap;
#else
typedef unsigned int tloc_size;
typedef unsigned int tloc_fl_bitmap;
#endif
#endif

#define TLOC_THREAD_SAFE

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

enum tloc__constants {
	tloc__MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
	tloc__MINIMUM_BLOCK_SIZE = 16,
	tloc__SECOND_LEVEL_INDEX_LOG2 = 5,
	tloc__SECOND_LEVEL_INDEX = 1 << tloc__SECOND_LEVEL_INDEX_LOG2,
	tloc__FIRST_LEVEL_INDEX_MAX = (1 << (MEMORY_ALIGNMENT_LOG2 + 3)) - 1,
	tloc__BLOCK_POINTER_OFFSET = sizeof(void*) + sizeof(tloc_size),
	tloc__BLOCK_SIZE_OVERHEAD = sizeof(tloc_size),
	tloc__POINTER_SIZE = sizeof(void*)
};

typedef enum tloc__boundary_tag_flags {
	tloc__BLOCK_IS_FREE = 1 << 0,
	tloc__PREV_BLOCK_IS_FREE = 1 << 1,
} tloc__boundary_tag_flags;

static const tloc_size tloc__BLOCK_IS_LOCKED = 1ULL << tloc__FIRST_LEVEL_INDEX_MAX;
static const tloc_size tloc__BLOCK_SIZE_MASK = ~(tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE | (1ULL << tloc__FIRST_LEVEL_INDEX_MAX));

typedef enum tloc__error_codes {
	tloc__OK,
	tloc__INVALID_FIRST_BLOCK,
	tloc__INVALID_BLOCK_FOUND,
	tloc__PHYSICAL_BLOCK_MISALIGNMENT,
	tloc__INVALID_SEGRATED_LIST,
	tloc__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST,
	tloc__SECOND_LEVEL_BITMAPS_NOT_INITIALISED
} tloc__error_codes;

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
	/*	Might not strictly need the following 3 items, I like them for convenience. */
	tloc_header *first_block;
	void *end_of_pool;
	size_t total_memory;
#if defined(TLOC_THREAD_SAFE)
	/* Multithreading */
	tloc_sl_bitmap *second_level_bitmap_locks;
#endif
	/*	Here we store all of the free block data. first_level_bitmap is either a 32bit int
	or 64bit depending on the mode you're in. second_level_bitmaps are an array of 32bit
	ints. segregated_lists is a two level array pointing to free blocks. We don't know
	how big these arrays are until we initialise the allocator as it will depend on how
	big the memory pool is as that determins how many classes there are.*/
	tloc_fl_bitmap first_level_bitmap;
	tloc_sl_bitmap *second_level_bitmaps;
	tloc_header ***segregated_lists;
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
static inline tloc_size tloc__compare_and_exchange_bitmap(volatile tloc_sl_bitmap* target, tloc_sl_bitmap value, tloc_sl_bitmap original) {
	return InterlockedCompareExchange(target, value, original);
}
static inline tloc_size tloc__exchange_bitmap(volatile tloc_sl_bitmap* target, tloc_sl_bitmap value) {
	return InterlockedExchange(target, value);
}
#if defined(tloc__64BIT)
static inline tloc_size tloc__compare_and_exchange(volatile tloc_size* target, tloc_size value, tloc_size original) {
	return InterlockedCompareExchange64(target, value, original);
}
static inline tloc_size tloc__exchange(volatile tloc_size* target, tloc_size value) {
	return InterlockedExchange64(target, value);
}
#else
static inline tloc_size tloc__compare_and_exchange(volatile tloc_size* target, tloc_size value, tloc_size original) {
	return InterlockedCompareExchange(target, value, orginal);
}
static inline tloc_size tloc__exchange(volatile tloc_size* target, tloc_size value) {
	return InterlockedExchange(target, value);
}
#endif
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

#endif

#if defined(TLOC_THREAD_SAFE)
#define tloc__UNLOCK_SECOND_LEVEL_INDEX(fli, sli) tloc__exchange_bitmap(&allocator->second_level_bitmap_locks[fli], allocator->second_level_bitmap_locks[fli] & ~(1 << sli))
#else
#define tloc__UNLOCK_SECOND_LEVEL_INDEX(fli)
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
	Reset an allocator back to it's initialised state. All allocations will be freed so make sure you don't try to reference or free any allocations
	after doing this.

	@param tloc_allocator*			A pointer to a tcoc_allocator that you want to reset
*/
TLOC_API void tloc_Reset(tloc_allocator *allocator);

/*
	Get the total size of the memory pool available for allocating after taking into account the allocator overhead

	@param tloc_allocator*			A pointer to a tcoc_allocator
	@returnst loc_size				This size of available pool memory
*/
TLOC_API tloc_size tloc_AllocatorPoolSize(tloc_allocator *allocator);

//--End of user functions

//Debugging
typedef void(*tloc__block_output)(void* ptr, size_t size, int used, void* user, int is_final_output);
//All Second level bitmaps should be initialised to 0
tloc__error_codes tloc_VerifySecondLevelBitmapsAreInitialised(tloc_allocator *allocator);
//A valid pointer in the context of an allocator is one that points within the bounds of the memory assigned to the allocator
tloc_bool tloc_ValidPointer(tloc_allocator *allocator, void *pointer);
//Checks all pointers within the block are valid
tloc_bool tloc_ValidBlock(tloc_allocator *allocator, tloc_header *block);
//Checks to see if the block links up correctly with neighbouring blocks
tloc_bool tloc_ConfirmBlockLink(tloc_allocator *allocator, tloc_header *block);
//Get a count of all blocks in the allocator, whether or not they're free or used.
int tloc_BlockCount(tloc_allocator *allocator);

//Private inline functions, user doesn't need to call these
static inline void tloc__map(tloc_size size, tloc_index *fli, tloc_index *sli) {
	*fli = tloc__scan_reverse(size);
	size = size & ~(1 << *fli);
	*sli = (tloc_index)(size >> (*fli - tloc__SECOND_LEVEL_INDEX_LOG2)) % tloc__SECOND_LEVEL_INDEX;
}

//Read only functions
static inline tloc_bool tloc__has_free_block(const tloc_allocator *allocator, tloc_index fli, tloc_index sli) {
	return allocator->first_level_bitmap & (1ULL << fli) && allocator->second_level_bitmaps[fli] & (1U << sli);
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

static inline tloc_size tloc__is_locked_block(const tloc_header *block) {
	return block->size & tloc__BLOCK_IS_LOCKED;
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

static inline void *tloc__end_of_memory_pointer(const tloc_allocator *allocator) {
	return (void*)((char*)allocator + allocator->total_memory);
}

static inline tloc_header *tloc__first_block(const tloc_allocator *allocator) {
	return allocator->first_block;
}

static inline tloc_size tloc__block_size(const tloc_header *block) {
	return block->size & tloc__BLOCK_SIZE_MASK;
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

static inline tloc_header *tloc__next_physical_block(const tloc_header *block) {
	return (tloc_header*)((char*)tloc__block_user_ptr(block) + tloc__block_size(block));
}

static inline tloc_bool tloc__is_last_block(const tloc_allocator *allocator, const tloc_header *block) {
	return (void*)tloc__next_physical_block(block) == allocator->end_of_pool;
}

static inline tloc_bool tloc__valid_allocation_pointer(const tloc_allocator *allocator, const tloc_header *block) {
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

static inline tloc_index tloc__find_next_size_up(tloc_fl_bitmap map, tloc_uint start) {
	//Mask out all bits up to the start point of the scan
	map &= (~0ULL << (start + 1));
	return tloc__scan_forward(map);
}

//Write functions
#if defined(TLOC_THREAD_SAFE)
static inline void tloc__wait_until_sli_is_free_and_lock(tloc_allocator *allocator, tloc_fl_bitmap fli, tloc_sl_bitmap sli) {
	for (;;) {
		tloc_sl_bitmap original_bitmap_locks = allocator->second_level_bitmap_locks[fli];
		if (!(original_bitmap_locks & (1ULL << fli))) {
			//If not locked then try and lock it
			tloc_sl_bitmap new_bitmap_locks = allocator->second_level_bitmap_locks[fli] & 1ULL << fli;
			tloc_fl_bitmap bitmap = tloc__compare_and_exchange_bitmap(&allocator->second_level_bitmap_locks[fli], new_bitmap_locks, original_bitmap_locks);
			if (bitmap == original_bitmap_locks) {
				//We've locked down this size class, break out of the spin
				break;
			}
		}
	}
}

static inline void tloc__wait_until_block_free_and_lock(tloc_header *block) {
	for (;;) {
		tloc_size original_block_size = block->size;
		if (!(original_block_size & tloc__BLOCK_IS_LOCKED)) {
			//If not locked then try and lock it
			tloc_size new_block_size = block->size | tloc__BLOCK_IS_LOCKED;
			tloc_size block_size = tloc__compare_and_exchange(&block->size, new_block_size, original_block_size);
			if (block_size != original_block_size) {
				//Already locked, keep spinning
				break;
			}
		}
	}
	//Successfully locked the blocks
}

static inline void tloc__unlock_block(tloc_header *block) {
	assert(block->size & tloc__BLOCK_IS_LOCKED);
	block->size &= ~tloc__BLOCK_IS_LOCKED;
}

static inline tloc_bool tloc__wait_until_blocks_free_and_lock(tloc_allocator *allocator, tloc_header *start_block, tloc_index block_count) {
	tloc_index index = 0;
	tloc_header *current_block = start_block;
	for (;;) {
		while (index != block_count) {
			if (!tloc__valid_block(allocator, current_block)) {
				//while waiting this block probably got merged with another block
				//Unlock anything that we locked already and try again
				for (tloc_index i = 0; i != index; ++i) {
					tloc__unlock_block(current_block);
				}
				return 0;
			}
			tloc_size original_block_size = current_block->size;
			if (!(original_block_size & tloc__BLOCK_IS_LOCKED)) {
				//If not locked then try and lock it
				tloc_size new_block_size = current_block->size | tloc__BLOCK_IS_LOCKED;
				tloc_size block_size = tloc__compare_and_exchange(&current_block->size, new_block_size, original_block_size);
				if (block_size != original_block_size) {
					//Already locked, keep spinning
					break;
				}
				if (!tloc__is_last_block(allocator, current_block)) {
					current_block = tloc__next_physical_block(current_block);
				}
				else {
					//Successfully locked the blocks
					return 1;
				}
				index++;
			}
		}
		if (index == block_count) {
			return 1;
		}
	}
	return 1;
	//Successfully locked the blocks
}

static inline void tloc__unlock_blocks(tloc_allocator *allocator, tloc_header *start_block, tloc_index block_count) {
	tloc_index index = 0;
	tloc_header *current_block = start_block;
	while (index != block_count) {
		//We own these blocks so can just go ahead and change
		current_block->size &= ~tloc__BLOCK_IS_LOCKED;
		if (!tloc__is_last_block(allocator, current_block)) {
			current_block = tloc__next_physical_block(current_block);
		}
		else {
			return;
		}
		index++;
	}
}
#endif

static inline void tloc__set_block_size(tloc_header *block, tloc_size size) {
#if defined(TLOC_THREAD_SAFE)
	tloc_size boundary_tag = block->size & (tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE | tloc__BLOCK_IS_LOCKED);
	block->size = size | boundary_tag;
#else
	tloc_size boundary_tag = block->size & (tloc__BLOCK_IS_FREE | tloc__PREV_BLOCK_IS_FREE);
	block->size = size | boundary_tag;
#endif
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

/*
	Push a block onto the segregated list of free blocks. Called when tloc_Free is called. Generally blocks are
	merged if possible before this is called
*/
static inline void tloc__push_block(tloc_allocator *allocator, tloc_header *block) {
	tloc_index fli;
	tloc_index sli;
	//Get the size class of the block
	tloc__map(tloc__block_size(block), &fli, &sli);
#if defined(TLOC_THREAD_SAFE)
	//Lock the next fli
	tloc__wait_until_sli_is_free_and_lock(allocator, fli, sli);
#endif
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
	allocator->first_level_bitmap |= 1ULL << fli;
	allocator->second_level_bitmaps[fli] |= 1 << sli;
	tloc__mark_block_as_free(allocator, block);
	tloc__UNLOCK_SECOND_LEVEL_INDEX(fli, sli);
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
	tloc__map(tloc__block_size(block), &fli, &sli);
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

/*
	This function is called when tloc_Allocate is called. Once a free block is found then it will be split
	if the size + header overhead + the minimum block size (16b) is greater then the size of the free block.
	If not then it simply returns the free block as it is without splitting. 
	If split then the trimmed amount is added back to the segregated list of free blocks.
*/
static inline void *tloc__maybe_split_block(tloc_allocator *allocator, tloc_header *block, tloc_size size) {
#if defined(TLOC_THREAD_SAFE)
	tloc__wait_until_blocks_free_and_lock(allocator, block, 2);
#endif
	tloc_size size_plus_overhead = size + tloc__BLOCK_POINTER_OFFSET;
	if (size_plus_overhead + tloc__MINIMUM_BLOCK_SIZE > tloc__block_size(block)) {
		return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
	}
	tloc_header *trimmed = (tloc_header*)((char*)tloc__block_user_ptr(block) + size);
	trimmed->size = 0;
	tloc__set_block_size(trimmed, tloc__block_size(block) - size_plus_overhead);
#if defined(TLOC_THREAD_SAFE)
	trimmed->size |= tloc__BLOCK_IS_LOCKED;
#endif
	if (!tloc__is_last_block(allocator, block)) {
		tloc_header *next_block = tloc__next_physical_block(block);
		tloc__set_prev_physical_block(next_block, trimmed);
	}
	tloc__set_prev_physical_block(trimmed, block);
	tloc__set_block_size(block, size);
	tloc__push_block(allocator, trimmed);
#if defined(TLOC_THREAD_SAFE)
	tloc__unlock_blocks(allocator, block, 3);
#endif
	return (void*)((char*)block + tloc__BLOCK_POINTER_OFFSET);
}

/*
	This function is called when tloc_Free is called and the previous physical block is free. If that's the case
	then this function will merge the block being freed with the previous physical block then add that back into 
	the segregated list of free blocks. Note that that happens in the tloc_Free function after attempting to merge
	both ways.
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
	This function might be called when tloc_Free is called to free a block. If the block being freed is not the last 
	physical block then this function is called and if the next block is free then it will be merged.
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
#if defined(TLOC_THREAD_SAFE)
	tloc_uint size_of_second_level_bitmap_locks_list = first_level_index_count * sizeof(tloc_index);
#else
	tloc_uint size_of_second_level_bitmap_locks_list = 0;
#endif
	tloc_uint size_of_each_second_level_list = second_level_index_count * tloc__POINTER_SIZE;
	tloc_uint size_of_first_level_list = first_level_index_count * tloc__POINTER_SIZE;
	tloc_uint segregated_list_size = (second_level_index_count * tloc__POINTER_SIZE * first_level_index_count) + (first_level_index_count * tloc__POINTER_SIZE);
	tloc_uint lists_size = segregated_list_size + size_of_second_level_bitmap_list + size_of_second_level_bitmap_locks_list;
	size_t minimum_size = lists_size + tloc__MINIMUM_BLOCK_SIZE + array_offset + tloc__BLOCK_POINTER_OFFSET;
	if (size < minimum_size) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Tried to initialise allocator with a memory pool that is too small. Must be at least: %zi bytes\n", TLOC_ERROR_NAME, minimum_size);
		return 0;
	}

	//Set the pointer to the start of the memory pool which starts after the segregated_lists array
	allocator->total_memory = size;
	allocator->first_block = (tloc_header*)((char*)memory + array_offset + lists_size);
	allocator->second_level_bitmaps = (tloc_sl_bitmap*)(((char*)memory) + array_offset);
#if defined(TLOC_THREAD_SAFE)
	allocator->second_level_bitmap_locks = (tloc_sl_bitmap*)(((char*)memory) + array_offset + size_of_second_level_bitmap_list);
	memset(allocator->second_level_bitmap_locks, 0, size_of_second_level_bitmap_locks_list);
#endif
	memset(allocator->second_level_bitmaps, 0, size_of_second_level_bitmap_list);
	assert(tloc_VerifySecondLevelBitmapsAreInitialised(allocator) == tloc__OK);
	//Point all of the segregated list array pointers to the empty block
	allocator->segregated_lists = (tloc_header***)(((char*)memory) + array_offset + size_of_second_level_bitmap_list + size_of_second_level_bitmap_locks_list);
	for (tloc_uint i = 0; i < first_level_index_count; i++) {
		tloc_header **ptr = (tloc_header**)((char*)memory + (array_offset + size_of_second_level_bitmap_list + size_of_first_level_list + size_of_second_level_bitmap_locks_list) + (i * size_of_each_second_level_list));
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
	//Make sure it aligns
	tloc__set_block_size(block, tloc__align_size_down(tloc__block_size(block), tloc__MEMORY_ALIGNMENT));
	allocator->end_of_pool = tloc__next_physical_block(block);
	assert(tloc__block_size(block) > tloc__MINIMUM_BLOCK_SIZE);
	block->size |= tloc__BLOCK_IS_FREE;
	block->prev_physical_block = &allocator->end_block;
	tloc__push_block(allocator, block);

	assert(tloc__is_last_block(allocator, block));

	block->prev_physical_block = tloc__end(allocator);

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

void tloc_Reset(tloc_allocator *allocator) {
	tloc_size size = allocator->total_memory;
	memset(allocator, 0, allocator->total_memory);
	tloc_InitialiseAllocator(allocator, size);
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
	tloc__map(size, &fli, &sli);

#if defined(TLOC_THREAD_SAFE)
	//Is this fli currently locked by another thread?
	//Do an atomic compare here and wait until the fli is free
	tloc__wait_until_sli_is_free_and_lock(allocator, fli, sli);
#endif
	//Note that there may well be an appropriate size block in the size class but that block may not be at the head of the list
	//In this situation we could opt to loop through the list of the size class to see if there is an appropriate size but instead
	//we stick to the paper and just move on to the next class up to keep a O1 speed at the cost of some extra fragmentation
	if (tloc__has_free_block(allocator, fli, sli) && tloc__block_size(allocator->segregated_lists[fli][sli]) >= size) {
		tloc__UNLOCK_SECOND_LEVEL_INDEX(fli, sli);
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
		tloc__UNLOCK_SECOND_LEVEL_INDEX(fli, sli);
		if (fli > -1) {
			sli = tloc__scan_forward(allocator->second_level_bitmaps[fli]);
#if defined(TLOC_THREAD_SAFE)
			//Lock the next fli
			tloc__wait_until_sli_is_free_and_lock(allocator, fli, sli);
#endif
			tloc_header *block = tloc__pop_block(allocator, fli, sli);
			assert(tloc_ConfirmBlockLink(allocator, block));
			assert(tloc__block_size(block) > size);
			void *allocation = tloc__maybe_split_block(allocator, block, size);
			tloc__UNLOCK_SECOND_LEVEL_INDEX(fli, sli);
			return allocation;
		}
	}
	else {
		tloc_header *block = tloc__pop_block(allocator, fli, sli);
		assert(tloc_ConfirmBlockLink(allocator, block));
		assert(tloc__block_size(block) > size);
		void *allocation = tloc__maybe_split_block(allocator, block, size);
		tloc__UNLOCK_SECOND_LEVEL_INDEX(fli, sli);
		return allocation;
	}
	//Out of memory;
	TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Not enough memory in pool to allocate %zu bytes\n", TLOC_ERROR_NAME, size);
	tloc__UNLOCK_SECOND_LEVEL_INDEX(fli, sli);
	return NULL;
}

int tloc_Free(tloc_allocator *allocator, void* allocation) {
	tloc_header *block = tloc__block_from_allocation(allocation);
#if defined(TLOC_THREAD_SAFE)
	if (!tloc__is_used_block(block)) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Tried to free an allocation that was already free. Make sure that another thread has not freed this block already.\n", TLOC_ERROR_NAME);
		return 0;
	}
#endif
	if (!tloc__valid_allocation_pointer(allocator, block)) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Tried to free an invalid allocation. Pointer doesn't point to a valid block in the allocator or potentially writes were made outside of the allocation bounds.\n", TLOC_ERROR_NAME);
		return 0;
	}
	if (!tloc__valid_block(allocator, block)) {
		TLOC_PRINT_ERROR(TLOC_ERROR_COLOR"%s: Memory corruption detected. Block you are trying to free no longer links with other blocks correctly suggesting that writes were made outside of the allocation bounds.\n", TLOC_ERROR_NAME);
		return 0;
	}
#if defined(TLOC_THREAD_SAFE)
	tloc_header *start_block = block->prev_physical_block;
	tloc_index block_count = 4;
	if (start_block != &allocator->end_block) {
		//Lock a potential 4 blocks down which is the maximum number of blocks we might change
		//when merging
		if (!tloc__wait_until_blocks_free_and_lock(allocator, start_block, block_count)) {
			//If the blocks became invalid while waiting then try calling again.
			return tloc_Free(allocator, allocation);
		}
	}
	else {
		start_block = block;
		block_count = 3;
		if (tloc__wait_until_blocks_free_and_lock(allocator, block, block_count)) {
			//If the blocks became invalid while waiting then try calling again.
			return tloc_Free(allocator, allocation);
		}
	}
	//If we get here then the blocks are locked to this process
	if (tloc__is_free_block(block)) {
		//Some other thread must have freed in the meantime
		tloc__unlock_blocks(allocator, start_block, block_count);
		return 1;
	}
	if (tloc__prev_is_free_block(block)) {
		assert(block->prev_physical_block);		//Must be a valid previous physical block
		if (tloc__is_free_block(block->prev_physical_block)) {
			block_count--;
			block = tloc__merge_with_prev_block(allocator, block);
		}
	}
	if (!tloc__is_last_block(allocator, block)) {
		block_count--;
		tloc__merge_with_next_block_if_free(allocator, block);
	}
	tloc__unlock_blocks(allocator, start_block, block_count);
	return 1;
#else
	if (tloc__prev_is_free_block(block)) {
		assert(block->prev_physical_block);		//Must be a valid previous physical block
		block = tloc__merge_with_prev_block(allocator, block);
	}
	if (!tloc__is_last_block(allocator, block)) {
		tloc__merge_with_next_block_if_free(allocator, block);
	}
#endif
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
