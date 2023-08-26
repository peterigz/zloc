
/*	Pocket Allocator, a Two Level Segregated Fit memory allocator

	This software is dual-licensed. See bottom of file for license details.
*/

#ifndef PKT_INCLUDE_H
#define PKT_INCLUDE_H

//#define PKT_DEV_MODE

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>

//Header
#define pkt__Min(a, b) (((a) < (b)) ? (a) : (b))
#define pkt__Max(a, b) (((a) > (b)) ? (a) : (b))

#ifndef PKT_API
#define PKT_API
#endif

typedef int pkt_index;
typedef unsigned int pkt_sl_bitmap;
typedef unsigned int pkt_uint;
typedef unsigned int pkt_thread_access;
typedef int pkt_bool;
typedef void* pkt_pool;

#if !defined (PKT_ASSERT)
#define PKT_ASSERT assert
#endif

#define pkt__is_pow2(x) ((x) && !((x) & ((x) - 1)))
#define pkt__glue2(x, y) x ## y
#define pkt__glue(x, y) pkt__glue2(x, y)
#define pkt__static_assert(exp) \
	typedef char pkt__glue(static_assert, __LINE__) [(exp) ? 1 : -1]

#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__x86_64__)
#define pkt__64BIT
typedef size_t pkt_size;
typedef size_t pkt_fl_bitmap;
#define PKT_ONE 1ULL
#else
typedef size_t pkt_size;
typedef size_t pkt_fl_bitmap;
#define PKT_ONE 1U
#endif

#ifndef MEMORY_ALIGNMENT_LOG2
#if defined(pkt__64BIT)
#define MEMORY_ALIGNMENT_LOG2 3		//64 bit
#else
#define MEMORY_ALIGNMENT_LOG2 2		//32 bit
#endif
#endif

#ifndef PKT_ERROR_NAME
#define PKT_ERROR_NAME "Allocator Error"
#endif

#ifndef PKT_ERROR_COLOR
#define PKT_ERROR_COLOR "\033[31m"
#endif

#ifdef PKT_OUTPUT_ERROR_MESSAGES
#include <stdio.h>
#define PKT_PRINT_ERROR(message_f, ...) printf(message_f"\033[0m", __VA_ARGS__)
#else
#define PKT_PRINT_ERROR(message_f, ...)
#endif

#define pkt__KILOBYTE(Value) ((Value) * 1024LL)
#define pkt__MEGABYTE(Value) (pkt__KILOBYTE(Value) * 1024LL)
#define pkt__GIGABYTE(Value) (pkt__MEGABYTE(Value) * 1024LL)

#ifndef PKT_MAX_SIZE_INDEX
#if defined(pkt__64BIT)
#define PKT_MAX_SIZE_INDEX 32
#else
#define PKT_MAX_SIZE_INDEX 30
#endif
#endif

pkt__static_assert(PKT_MAX_SIZE_INDEX < 64);

#ifdef __cplusplus
extern "C" {
#endif

#define pkt__MAXIMUM_BLOCK_SIZE (PKT_ONE << PKT_MAX_SIZE_INDEX)

	enum pkt__constants {
		pkt__MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
		pkt__MINIMUM_POOL_SIZE = pkt__MEGABYTE(1),
		pkt__SECOND_LEVEL_INDEX_LOG2 = 5,
		pkt__FIRST_LEVEL_INDEX_COUNT = PKT_MAX_SIZE_INDEX,
		pkt__SECOND_LEVEL_INDEX_COUNT = 1 << pkt__SECOND_LEVEL_INDEX_LOG2,
		pkt__FIRST_LEVEL_INDEX_MAX = (1 << (MEMORY_ALIGNMENT_LOG2 + 3)) - 1,
		pkt__BLOCK_POINTER_OFFSET = sizeof(void*) + sizeof(pkt_size),
		pkt__MINIMUM_BLOCK_SIZE = 16,
		pkt__BLOCK_SIZE_OVERHEAD = sizeof(pkt_size),
		pkt__POINTER_SIZE = sizeof(void*)
	};

	typedef enum pkt__boundary_tag_flags {
		pkt__BLOCK_IS_FREE = 1 << 0,
		pkt__PREV_BLOCK_IS_FREE = 1 << 1,
	} pkt__boundary_tag_flags;

	typedef enum pkt__error_codes {
		pkt__OK,
		pkt__INVALID_FIRST_BLOCK,
		pkt__INVALID_BLOCK_FOUND,
		pkt__PHYSICAL_BLOCK_MISALIGNMENT,
		pkt__INVALID_SEGRATED_LIST,
		pkt__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST,
		pkt__SECOND_LEVEL_BITMAPS_NOT_INITIALISED
	} pkt__error_codes;

	typedef enum pkt__thread_ops {
		pkt__FREEING_BLOCK = 1 << 0,
		pkt__ALLOCATING_BLOCK = 1 << 1
	} pkt__thread_ops;

	/*
		Each block has a header that if used only has a pointer to the previous physical block
		and the size. If the block is free then the prev and next free blocks are also stored.
	*/
	typedef struct pkt_header {
		struct pkt_header *prev_physical_block;
		/*	Note that the size is either 4 or 8 bytes aligned so the boundary tag (2 flags denoting
			whether this or the previous block is free) can be stored in the first 2 least
			significant bits	*/
		pkt_size size;
		/*
		User allocation will start here when the block is used. When the block is free prev and next
		are pointers in a linked list of free blocks within the same class size of blocks
		*/
		struct pkt_header *prev_free_block;
		struct pkt_header *next_free_block;
	} pkt_header;

	typedef struct pkt_allocator {
		/*	This is basically a terminator block that free blocks can point to if they're at the end
			of a free list. */
		pkt_header null_block;
#if defined(PKT_THREAD_SAFE)
		/* Multithreading protection*/
		volatile pkt_thread_access access;
		volatile pkt_thread_access access_override;
#endif
#if defined(PKT_ENABLE_REMOTE_MEMORY)
		void *user_data;
		pkt_size(*get_block_size_callback)(const pkt_header* block);
		void(*merge_next_callback)(void *user_data, pkt_header* block, pkt_header *next_block);
		void(*merge_prev_callback)(void *user_data, pkt_header* prev_block, pkt_header *block);
		void(*split_block_callback)(void *user_data, pkt_header* block, pkt_header* trimmed_block, pkt_size remote_size);
		void(*add_pool_callback)(void *user_data, void* block_extension);
		void(*unable_to_reallocate_callback)(void *user_data, pkt_header *block, pkt_header *new_block);
		pkt_size block_extension_size;
#endif
		pkt_size minimum_allocation_size;
		/*	Here we store all of the free block data. first_level_bitmap is either a 32bit int
		or 64bit depending on whether pkt__64BIT is set. Second_level_bitmaps are an array of 32bit
		ints. segregated_lists is a two level array pointing to free blocks or null_block if the list
		is empty. */
		pkt_fl_bitmap first_level_bitmap;
		pkt_sl_bitmap second_level_bitmaps[pkt__FIRST_LEVEL_INDEX_COUNT];
		pkt_header *segregated_lists[pkt__FIRST_LEVEL_INDEX_COUNT][pkt__SECOND_LEVEL_INDEX_COUNT];
	} pkt_allocator;

#if defined(PKT_ENABLE_REMOTE_MEMORY)
	/*
	A minimal remote header block. You can define your own header to store additional information but it must include
	size and memory_offset in the first 2 fields.
	*/
	typedef struct pkt_remote_header {
		pkt_size size;
		pkt_size memory_offset;
	} pkt_remote_header;

#define pkt__map_size (remote_size ? remote_size : size)
#define pkt__do_size_class_callback(block) allocator->get_block_size_callback(block)
#define pkt__do_merge_next_callback allocator->merge_next_callback(allocator->user_data, block, next_block)
#define pkt__do_merge_prev_callback allocator->merge_prev_callback(allocator->user_data, prev_block, block)
#define pkt__do_split_block_callback allocator->split_block_callback(allocator->user_data, block, trimmed, remote_size)
#define pkt__do_add_pool_callback allocator->add_pool_callback(allocator->user_data, block)
#define pkt__do_unable_to_reallocate_callback pkt_header *new_block = pkt__block_from_allocation(allocation); pkt_header *block = pkt__block_from_allocation(ptr); allocator->unable_to_reallocate_callback(allocator->user_data, block, new_block)
#define pkt__block_extension_size (allocator->block_extension_size & ~1)
#define pkt__call_maybe_split_block pkt__maybe_split_block(allocator, block, size, remote_size) 
#else
#define pkt__map_size size
#define pkt__do_size_class_callback(block) pkt__block_size(block)
#define pkt__do_merge_next_callback
#define pkt__do_merge_prev_callback
#define pkt__do_split_block_callback
#define pkt__do_add_pool_callback
#define pkt__do_unable_to_reallocate_callback
#define pkt__block_extension_size 0
#define pkt__call_maybe_split_block pkt__maybe_split_block(allocator, block, size, 0) 
#endif

#if defined (_MSC_VER) && (_MSC_VER >= 1400) && (defined (_M_IX86) || defined (_M_X64))
	/* Microsoft Visual C++ support on x86/X64 architectures. */

#include <intrin.h>

	static inline int pkt__scan_reverse(pkt_size bitmap) {
		unsigned long index;
#if defined(pkt__64BIT)
		return _BitScanReverse64(&index, bitmap) ? index : -1;
#else
		return _BitScanReverse(&index, bitmap) ? index : -1;
#endif
	}

	static inline int pkt__scan_forward(pkt_size bitmap)
	{
		unsigned long index;
#if defined(pkt__64BIT)
		return _BitScanForward64(&index, bitmap) ? index : -1;
#else
		return _BitScanForward(&index, bitmap) ? index : -1;
#endif
	}

#ifdef _WIN32
#include <Windows.h>
	static inline pkt_thread_access pkt__compare_and_exchange(volatile pkt_thread_access* target, pkt_thread_access value, pkt_thread_access original) {
		return InterlockedCompareExchange(target, value, original);
	}
#endif

#elif defined(__GNUC__) && ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)) && \
      (defined(__i386__) || defined(__x86_64__)) || defined(__clang__)
	/* GNU C/C++ or Clang support on x86/x64 architectures. */

	static inline int pkt__scan_reverse(pkt_size bitmap)
	{
#if defined(pkt__64BIT)
		return 64 - __builtin_clzll(bitmap) - 1;
#else
		return 32 - __builtin_clz((int)bitmap) - 1;
#endif
	}

	static inline int pkt__scan_forward(pkt_size bitmap)
	{
#if defined(pkt__64BIT)
		return __builtin_ffsll(bitmap) - 1;
#else
		return __builtin_ffs((int)bitmap) - 1;
#endif
	}

	static inline pkt_thread_access pkt__compare_and_exchange(volatile pkt_thread_access* target, pkt_thread_access value, pkt_thread_access original) {
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
		@param	pkt_size				The size of the memory you're passing
		@returns pkt_allocator*			A pointer to a pkt_allocator which you'll need to use when calling pkt_Allocate or pkt_Free. Note that
										this pointer will be the same address as the memory you're passing in as all the information the allocator
										stores to organise memory blocks is stored at the beginning of the memory.
										If something went wrong then 0 is returned. Define PKT_OUTPUT_ERROR_MESSAGES before including this header
										file to see any errors in the console.
	*/
	PKT_API pkt_allocator *pkt_InitialiseAllocator(void *memory);

	/*
		Initialise an allocator and a pool at the same time. The data stucture to store the allocator will be stored at the beginning of the memory
		you pass to the function and the remaining memory will be used as the pool.

		@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
		@param	pkt_size				The size of the memory you're passing
		@returns pkt_allocator*		A pointer to a pkt_allocator which you'll need to use when calling pkt_Allocate or pkt_Free.
										If something went wrong then 0 is returned. Define PKT_OUTPUT_ERROR_MESSAGES before including this header
										file to see any errors in the console.
	*/
	PKT_API pkt_allocator *pkt_InitialiseAllocatorWithPool(void *memory, pkt_size size);

	/*
		Add a new memory pool to the allocator. Pools don't have to all be the same size, adding a pool will create the biggest block it can within
		the pool and then add that to the segregated list of free blocks in the allocator. All the pools in the allocator will be naturally linked
		together in the segregated list because all blocks are linked together with a linked list either as physical neighbours or free blocks in
		the segregated list.

		@param	pkt_allocator*			A pointer to some previously initialised allocator
		@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
		@param	pkt_size				The size of the memory you're passing
		@returns pkt_pool*				A pointer to the pool
	*/
	PKT_API pkt_pool *pkt_AddPool(pkt_allocator *allocator, void *memory, pkt_size size);

	/*
		Get the structure size of an allocator. You can use this to take into account the overhead of the allocator when preparing a new allocator
		with memory pool.

		@returns pkt_size				The struct size of the allocator in bytes
	*/
	PKT_API pkt_size pkt_AllocatorSize(void);

	/*
		If you initialised an allocator with a pool then you can use this function to get a pointer to the start of the pool. It won't get a pointer
		to any other pool in the allocator. You can just get that when you call pkt_AddPool.

		@param	pkt_allocator*			A pointer to some previously initialised allocator
		@returns pkt_pool				A pointer to the pool memory in the allocator
	*/
	PKT_API pkt_pool *pkt_GetPool(pkt_allocator *allocator);

	/*
		Allocate some memory within a pkt_allocator of the specified size. Minimum size is 16 bytes.

		@param	pkt_allocator			A pointer to an initialised pkt_allocator
		@param	pkt_size				The size of the memory you're passing
		@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to
										no free memory. If that happens then you may want to add a pool at that point.
	*/
	PKT_API void *pkt_Allocate(pkt_allocator *allocator, pkt_size size);

	/*
		Try to reallocate an existing memory block within the allocator. If possible the current block will be merged with the physical neigbouring
		block, otherwise a normal pkt_Allocate will take place and the data copied over to the new allocation.

		@param	pkt_size				The size of the memory you're passing
		@param	void*					A ptr to the current memory you're reallocating
		@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to
										no free memory. If that happens then you may want to add a pool at that point.
	*/
	PKT_API void *pkt_Reallocate(pkt_allocator *allocator, void *ptr, pkt_size size);

	/*
		Free an allocation from a pkt_allocator. When freeing a block of memory any adjacent free blocks are merged together to keep on top of
		fragmentation as much as possible. A check is also done to confirm that the block being freed is still valid and detect any memory corruption
		due to out of bounds writing of this or potentially other blocks.

		It's recommended to call this function with an assert: PKT_ASSERT(pkt_Free(allocator, allocation));
		An error is also output to console as long as PKT_OUTPUT_ERROR_MESSAGES is defined.

		@returns int		returns 1 if the allocation was successfully freed, 0 otherwise.
	*/
	PKT_API int pkt_Free(pkt_allocator *allocator, void *allocation);

	/*
		Remove a pool from an allocator. Note that all blocks in the pool must be free and therefore all merged together into one block (this happens
		automatically as all blocks are freed are merged together into bigger blocks.

		@param pkt_allocator*			A pointer to a tcoc_allocator that you want to reset
		@param pkt_allocator*			A pointer to the memory pool that you want to free. You get this pointer when you add a pool to the allocator.
	*/
	PKT_API pkt_bool pkt_RemovePool(pkt_allocator *allocator, pkt_pool *pool);

	/*
	When using an allocator for managing remote memory, you need to set the bytes per block that a block storing infomation about the remote
	memory allocation will manage. For example you might set the value to 1MB so if you were to then allocate 4MB of remote memory then 4 blocks
	worth of space would be used to allocate that memory. This means that if it were to be freed and then split down to a smaller size they'd be
	enough blocks worth of space to do this.

	Note that the lower the number the more memory you need to track remote memory blocks but the more granular it will be. It will depend alot
	on the size of allocations you will need

	@param pkt_allocator*			A pointer to an initialised allocator
	@param pkt_size				The bytes per block you want it to be set to. Must be a power of 2
*/
	PKT_API void pkt_SetMinimumAllocationSize(pkt_allocator *allocator, pkt_size size);

#if defined(PKT_ENABLE_REMOTE_MEMORY)
	/*
		Initialise an allocator and a pool at the same time and flag it for use as a remote memory manager.
		The data stucture to store the allocator will be stored at the beginning of the memory you pass to the function and the remaining memory will
		be used as the pool. Use with pkt_CalculateRemoteBlockPoolSize to allow for the number of memory ranges you might need to manage in the
		remote memory pool(s)

		@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
		@param	pkt_size				The size of the memory you're passing
		@returns pkt_allocator*		A pointer to a pkt_allocator which you'll need to use when calling pkt_AllocateRemote or pkt_FreeRemote.
										If something went wrong then 0 is returned. Define PKT_OUTPUT_ERROR_MESSAGES before including this header
										file to see any errors in the console.
	*/
	PKT_API pkt_allocator *pkt_InitialiseAllocatorForRemote(void *memory);

	/*
		When using an allocator for managing remote memory, you need to set the size of the struct that you will be using to store information about
		the remote block of memory. This will be like an extension the existing pkt_header.

		@param pkt_allocator*			A pointer to an initialised allocator
		@param pkt_size				The size of the block extension. Will be aligned up to pkt__MEMORY_ALIGNMENT
	*/
	PKT_API void pkt_SetBlockExtensionSize(pkt_allocator *allocator, pkt_size size);

	/*
		Free a remote allocation from a pkt_allocator. You must have set up merging callbacks so that you can update your block extensions with the
		necessary buffer sizes and offsets

		It's recommended to call this function with an assert: PKT_ASSERT(pkt_FreeRemote(allocator, allocation));
		An error is also output to console as long as PKT_OUTPUT_ERROR_MESSAGES is defined.

		@returns int		returns 1 if the allocation was successfully freed, 0 otherwise.
	*/
	PKT_API int pkt_FreeRemote(pkt_allocator *allocator, void *allocation);

	/*
		Allocate some memory in a remote location from the normal heap. This is generally for allocating GPU memory.

		@param	pkt_allocator			A pointer to an initialised pkt_allocator
		@param	pkt_size				The size of the memory you're passing which should be the size of the block with the information about the
										buffer you're creating in the remote location
		@param	pkt_size				The remote size of the memory you're passing
		@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to
										no free memory. If that happens then you may want to add a pool at that point.
	*/
	PKT_API void *pkt_AllocateRemote(pkt_allocator *allocator, pkt_size remote_size);

	/*
		Get the size of a block plus the block extension size so that you can use this to create an allocator pool to store all the blocks that will
		track the remote memory. Be sure that you have already called and set the block extension size with pkt_SetBlockExtensionSize.

		@param	pkt_allocator			A pointer to an initialised pkt_allocator
		@returns pkt_size				The size of the block
	*/
	PKT_API pkt_size pkt_CalculateRemoteBlockPoolSize(pkt_allocator *allocator, pkt_size remote_pool_size);

	PKT_API void pkt_AddRemotePool(pkt_allocator *allocator, void *block_memory, pkt_size block_memory_size, pkt_size remote_pool_size);

	PKT_API inline void* pkt_BlockUserExtensionPtr(const pkt_header *block) {
		return (char*)block + sizeof(pkt_header);
	}

	PKT_API inline void* pkt_AllocationFromExtensionPtr(const void *block) {
		return (void*)((char*)block - pkt__MINIMUM_BLOCK_SIZE);
	}

#endif

	//--End of user functions

	//Private inline functions, user doesn't need to call these

	static inline void pkt__map(pkt_size size, pkt_index *fli, pkt_index *sli) {
		*fli = pkt__scan_reverse(size);
		size = size & ~(1 << *fli);
		*sli = (pkt_index)(size >> (*fli - pkt__SECOND_LEVEL_INDEX_LOG2)) % pkt__SECOND_LEVEL_INDEX_COUNT;
	}

#if defined(PKT_ENABLE_REMOTE_MEMORY)
	static inline void pkt__null_merge_callback(void *user_data, pkt_header *block1, pkt_header *block2) { return; }
	void pkt__remote_merge_next_callback(void *user_data, pkt_header *block1, pkt_header *block2);
	void pkt__remote_merge_prev_callback(void *user_data, pkt_header *block1, pkt_header *block2);
	pkt_size pkt__get_remote_size(const pkt_header *block1);
	static inline void pkt__null_split_callback(void *user_data, pkt_header *block, pkt_header *trimmed, pkt_size remote_size) { return; }
	static inline void pkt__null_add_pool_callback(void *user_data, void *block) { return; }
	static inline void pkt__null_unable_to_reallocate_callback(void *user_data, pkt_header *block, pkt_header *new_block) { return; }
	static inline void pkt__unset_remote_block_limit_reached(pkt_allocator *allocator) { allocator->block_extension_size &= ~1; };
#endif

	//Read only functions
	static inline pkt_bool pkt__has_free_block(const pkt_allocator *allocator, pkt_index fli, pkt_index sli) {
		return allocator->first_level_bitmap & (PKT_ONE << fli) && allocator->second_level_bitmaps[fli] & (1U << sli);
	}

	static inline pkt_bool pkt__is_used_block(const pkt_header *block) {
		return !(block->size & pkt__BLOCK_IS_FREE);
	}

	static inline pkt_bool pkt__is_free_block(const pkt_header *block) {
		return block->size & pkt__BLOCK_IS_FREE;
	}

	static inline pkt_bool pkt__prev_is_free_block(const pkt_header *block) {
		return block->size & pkt__PREV_BLOCK_IS_FREE;
	}

	static inline pkt_bool pkt__is_aligned(pkt_size size, pkt_size alignment) {
		return (size % alignment) == 0;
	}

	static inline pkt_size pkt__align_size_down(pkt_size size, pkt_size alignment) {
		return size - (size % alignment);
	}

	static inline pkt_size pkt__align_size_up(pkt_size size, pkt_size alignment) {
		pkt_size remainder = size % alignment;
		if (remainder != 0) {
			size += alignment - remainder;
		}
		return size;
	}

	static inline pkt_size pkt__adjust_size(pkt_size size, pkt_size minimum_size, pkt_index alignment) {
		return pkt__Min(pkt__Max(pkt__align_size_up(size, alignment), minimum_size), pkt__MAXIMUM_BLOCK_SIZE);
	}

	static inline pkt_size pkt__block_size(const pkt_header *block) {
		return block->size & ~(pkt__BLOCK_IS_FREE | pkt__PREV_BLOCK_IS_FREE);
	}

	static inline pkt_header *pkt__block_from_allocation(const void *allocation) {
		return (pkt_header*)((char*)allocation - pkt__BLOCK_POINTER_OFFSET);
	}

	static inline pkt_header *pkt__null_block(pkt_allocator *allocator) {
		return &allocator->null_block;
	}

	static inline void* pkt__block_user_ptr(const pkt_header *block) {
		return (char*)block + pkt__BLOCK_POINTER_OFFSET;
	}

	static inline pkt_header* pkt__first_block_in_pool(const pkt_pool *pool) {
		return (pkt_header*)((char*)pool - pkt__POINTER_SIZE);
	}

	static inline pkt_header *pkt__next_physical_block(const pkt_header *block) {
		return (pkt_header*)((char*)pkt__block_user_ptr(block) + pkt__block_size(block));
	}

	static inline pkt_bool pkt__next_block_is_free(const pkt_header *block) {
		return pkt__is_free_block(pkt__next_physical_block(block));
	}

	static inline pkt_header *pkt__allocator_first_block(pkt_allocator *allocator) {
		return (pkt_header*)((char*)allocator + pkt_AllocatorSize() - pkt__POINTER_SIZE);
	}

	static inline pkt_bool pkt__is_last_block_in_pool(const pkt_header *block) {
		return pkt__block_size(block) == 0;
	}

	static inline pkt_index pkt__find_next_size_up(pkt_fl_bitmap map, pkt_uint start) {
		//Mask out all bits up to the start point of the scan
		map &= (~0ULL << (start + 1));
		return pkt__scan_forward(map);
	}

	//Write functions
#if defined(PKT_THREAD_SAFE)

#define pkt__lock_thread_access												\
	if (allocator->access + allocator->access_override != 2) {					\
		do {																	\
		} while (0 != pkt__compare_and_exchange(&allocator->access, 1, 0));	\
	}																			\
	PKT_ASSERT(allocator->access != 0);

#define pkt__unlock_thread_access allocator->access_override = 0; allocator->access = 0;
#define pkt__access_override allocator->access_override = 1;

#else

#define pkt__lock_thread_access
#define pkt__unlock_thread_access 
#define pkt__access_override

#endif
	void *pkt__allocate(pkt_allocator *allocator, pkt_size size, pkt_size remote_size);

	static inline void pkt__set_block_size(pkt_header *block, pkt_size size) {
		pkt_size boundary_tag = block->size & (pkt__BLOCK_IS_FREE | pkt__PREV_BLOCK_IS_FREE);
		block->size = size | boundary_tag;
	}

	static inline void pkt__set_prev_physical_block(pkt_header *block, pkt_header *prev_block) {
		block->prev_physical_block = prev_block;
	}

	static inline void pkt__zero_block(pkt_header *block) {
		block->prev_physical_block = 0;
		block->size = 0;
	}

	static inline void pkt__mark_block_as_used(pkt_header *block) {
		block->size &= ~pkt__BLOCK_IS_FREE;
		pkt_header *next_block = pkt__next_physical_block(block);
		next_block->size &= ~pkt__PREV_BLOCK_IS_FREE;
	}

	static inline void pkt__mark_block_as_free(pkt_header *block) {
		block->size |= pkt__BLOCK_IS_FREE;
		pkt_header *next_block = pkt__next_physical_block(block);
		next_block->size |= pkt__PREV_BLOCK_IS_FREE;
	}

	static inline void pkt__block_set_used(pkt_header *block) {
		block->size &= ~pkt__BLOCK_IS_FREE;
	}

	static inline void pkt__block_set_free(pkt_header *block) {
		block->size |= pkt__BLOCK_IS_FREE;
	}

	static inline void pkt__block_set_prev_used(pkt_header *block) {
		block->size &= ~pkt__PREV_BLOCK_IS_FREE;
	}

	static inline void pkt__block_set_prev_free(pkt_header *block) {
		block->size |= pkt__PREV_BLOCK_IS_FREE;
	}

	/*
		Push a block onto the segregated list of free blocks. Called when pkt_Free is called. Generally blocks are
		merged if possible before this is called
	*/
	static inline void pkt__push_block(pkt_allocator *allocator, pkt_header *block) {
		pkt_index fli;
		pkt_index sli;
		//Get the size class of the block
		pkt__map(pkt__do_size_class_callback(block), &fli, &sli);
		pkt_header *current_block_in_free_list = allocator->segregated_lists[fli][sli];
		//Insert the block into the list by updating the next and prev free blocks of
		//this and the current block in the free list. The current block in the free
		//list may well be the null_block in the allocator so this just means that this
		//block will be added as the first block in this class of free blocks.
		block->next_free_block = current_block_in_free_list;
		block->prev_free_block = &allocator->null_block;
		current_block_in_free_list->prev_free_block = block;

		allocator->segregated_lists[fli][sli] = block;
		//Flag the bitmaps to mark that this size class now contains a free block
		allocator->first_level_bitmap |= PKT_ONE << fli;
		allocator->second_level_bitmaps[fli] |= 1 << sli;
		pkt__mark_block_as_free(block);
	}

	/*
		Remove a block from the segregated list in the allocator and return it. If there is a next free block in the size class
		then move it down the list, otherwise unflag the bitmaps as necessary. This is only called when we're trying to allocate
		some memory with pkt_Allocate and we've determined that there's a suitable free block in segregated_lists.
	*/
	static inline pkt_header *pkt__pop_block(pkt_allocator *allocator, pkt_index fli, pkt_index sli) {
		pkt_header *block = allocator->segregated_lists[fli][sli];

		//If the block in the segregated list is actually the null_block then something went very wrong.
		//Somehow the segregated lists had the end block assigned but the first or second level bitmaps
		//did not have the masks assigned
		PKT_ASSERT(block != &allocator->null_block);
		if (block->next_free_block != &allocator->null_block) {
			//If there are more free blocks in this size class then shift the next one down and terminate the prev_free_block
			allocator->segregated_lists[fli][sli] = block->next_free_block;
			allocator->segregated_lists[fli][sli]->prev_free_block = pkt__null_block(allocator);
		}
		else {
			//There's no more free blocks in this size class so flag the second level bitmap for this class to 0.
			allocator->segregated_lists[fli][sli] = pkt__null_block(allocator);
			allocator->second_level_bitmaps[fli] &= ~(1 << sli);
			if (allocator->second_level_bitmaps[fli] == 0) {
				//And if the second level bitmap is 0 then the corresponding bit in the first lebel can be zero'd too.
				allocator->first_level_bitmap &= ~(PKT_ONE << fli);
			}
		}
		pkt__mark_block_as_used(block);
		return block;
	}

	/*
		Remove a block from the segregated list. This is only called when we're merging blocks together. The block is
		just removed from the list and marked as used and then merged with an adjacent block.
	*/
	static inline void pkt__remove_block_from_segregated_list(pkt_allocator *allocator, pkt_header *block) {
		pkt_index fli, sli;
		//Get the size class
		pkt__map(pkt__do_size_class_callback(block), &fli, &sli);
		pkt_header *prev_block = block->prev_free_block;
		pkt_header *next_block = block->next_free_block;
		PKT_ASSERT(prev_block);
		PKT_ASSERT(next_block);
		next_block->prev_free_block = prev_block;
		prev_block->next_free_block = next_block;
		if (allocator->segregated_lists[fli][sli] == block) {
			allocator->segregated_lists[fli][sli] = next_block;
			if (next_block == pkt__null_block(allocator)) {
				allocator->second_level_bitmaps[fli] &= ~(1U << sli);
				if (allocator->second_level_bitmaps[fli] == 0) {
					allocator->first_level_bitmap &= ~(1ULL << fli);
				}
			}
		}
		pkt__mark_block_as_used(block);
	}

	/*
		This function is called when pkt_Allocate is called. Once a free block is found then it will be split
		if the size + header overhead + the minimum block size (16b) is greater then the size of the free block.
		If not then it simply returns the free block as it is without splitting.
		If split then the trimmed amount is added back to the segregated list of free blocks.
	*/
	static inline void *pkt__maybe_split_block(pkt_allocator *allocator, pkt_header *block, pkt_size size, pkt_size remote_size) {
		PKT_ASSERT(!pkt__is_last_block_in_pool(block));
		pkt_size size_plus_overhead = size + pkt__BLOCK_POINTER_OFFSET + pkt__block_extension_size;
		if (size_plus_overhead + pkt__MINIMUM_BLOCK_SIZE >= pkt__block_size(block) - pkt__block_extension_size) {
			return (void*)((char*)block + pkt__BLOCK_POINTER_OFFSET);
		}
		pkt_header *trimmed = (pkt_header*)((char*)pkt__block_user_ptr(block) + size + pkt__block_extension_size);
		trimmed->size = 0;
		pkt__set_block_size(trimmed, pkt__block_size(block) - size_plus_overhead);
		pkt_header *next_block = pkt__next_physical_block(block);
		pkt__set_prev_physical_block(next_block, trimmed);
		pkt__set_prev_physical_block(trimmed, block);
		pkt__set_block_size(block, size + pkt__block_extension_size);
		pkt__do_split_block_callback;
		pkt__push_block(allocator, trimmed);
		return (void*)((char*)block + pkt__BLOCK_POINTER_OFFSET);
	}

	/*
		This function is called when pkt_Free is called and the previous physical block is free. If that's the case
		then this function will merge the block being freed with the previous physical block then add that back into
		the segregated list of free blocks. Note that that happens in the pkt_Free function after attempting to merge
		both ways.
	*/
	static inline pkt_header *pkt__merge_with_prev_block(pkt_allocator *allocator, pkt_header *block) {
		PKT_ASSERT(!pkt__is_last_block_in_pool(block));
		pkt_header *prev_block = block->prev_physical_block;
		pkt__remove_block_from_segregated_list(allocator, prev_block);
		pkt__do_merge_prev_callback;
		pkt__set_block_size(prev_block, pkt__block_size(prev_block) + pkt__block_size(block) + pkt__BLOCK_POINTER_OFFSET);
		pkt_header *next_block = pkt__next_physical_block(block);
		pkt__set_prev_physical_block(next_block, prev_block);
		pkt__zero_block(block);
		return prev_block;
	}

	/*
		This function might be called when pkt_Free is called to free a block. If the block being freed is not the last
		physical block then this function is called and if the next block is free then it will be merged.
	*/
	static inline void pkt__merge_with_next_block(pkt_allocator *allocator, pkt_header *block) {
		pkt_header *next_block = pkt__next_physical_block(block);
		PKT_ASSERT(next_block->prev_physical_block == block);	//could be potentional memory corruption. Check that you're not writing outside the boundary of the block size
		PKT_ASSERT(!pkt__is_last_block_in_pool(next_block));
		pkt__remove_block_from_segregated_list(allocator, next_block);
		pkt__do_merge_next_callback;
		pkt__set_block_size(block, pkt__block_size(next_block) + pkt__block_size(block) + pkt__BLOCK_POINTER_OFFSET);
		pkt_header *block_after_next = pkt__next_physical_block(next_block);
		pkt__set_prev_physical_block(block_after_next, block);
		pkt__zero_block(next_block);
	}
	//--End of internal functions

	//--End of header declarations

#ifdef __cplusplus
}
#endif

#endif

//Implementation
#if defined(PKT_IMPLEMENTATION) || defined(PKT_DEV_MODE)

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

//Definitions
pkt_allocator *pkt_InitialiseAllocator(void *memory) {
	if (!memory) {
		PKT_PRINT_ERROR(PKT_ERROR_COLOR"%s: The memory pointer passed in to the initialiser was NULL, did it allocate properly?\n", PKT_ERROR_NAME);
		return 0;
	}

	pkt_allocator *allocator = (pkt_allocator*)memory;
	memset(allocator, 0, sizeof(pkt_allocator));
	allocator->null_block.next_free_block = &allocator->null_block;
	allocator->null_block.prev_free_block = &allocator->null_block;
	allocator->minimum_allocation_size = pkt__MINIMUM_BLOCK_SIZE;

	//Point all of the segregated list array pointers to the empty block
	for (pkt_uint i = 0; i < pkt__FIRST_LEVEL_INDEX_COUNT; i++) {
		for (pkt_uint j = 0; j < pkt__SECOND_LEVEL_INDEX_COUNT; j++) {
			allocator->segregated_lists[i][j] = &allocator->null_block;
		}
	}

#if defined(PKT_ENABLE_REMOTE_MEMORY)
	allocator->get_block_size_callback = pkt__block_size;
	allocator->merge_next_callback = pkt__null_merge_callback;
	allocator->merge_prev_callback = pkt__null_merge_callback;
	allocator->split_block_callback = pkt__null_split_callback;
	allocator->add_pool_callback = pkt__null_add_pool_callback;
	allocator->unable_to_reallocate_callback = pkt__null_unable_to_reallocate_callback;
#endif

	return allocator;
}

pkt_allocator *pkt_InitialiseAllocatorWithPool(void *memory, pkt_size size) {
	pkt_size array_offset = sizeof(pkt_allocator);
	if (size < array_offset + pkt__MEMORY_ALIGNMENT) {
		PKT_PRINT_ERROR(PKT_ERROR_COLOR"%s: Tried to initialise allocator with a memory allocation that is too small. Must be at least: %zi bytes\n", PKT_ERROR_NAME, array_offset + pkt__MEMORY_ALIGNMENT);
		return 0;
	}

	pkt_allocator *allocator = pkt_InitialiseAllocator(memory);
	if (!allocator) {
		return 0;
	}
	pkt_AddPool(allocator, pkt_GetPool(allocator), size - pkt_AllocatorSize());
	return allocator;
}

pkt_size pkt_AllocatorSize(void) {
	return sizeof(pkt_allocator);
}

void pkt_SetMinimumAllocationSize(pkt_allocator *allocator, pkt_size size) {
	PKT_ASSERT(allocator->minimum_allocation_size == pkt__MINIMUM_BLOCK_SIZE);		//You cannot change this once set
	PKT_ASSERT(pkt__is_pow2(size));													//Size must be a power of 2
	allocator->minimum_allocation_size = pkt__Max(pkt__MINIMUM_BLOCK_SIZE, size);
}

pkt_pool *pkt_GetPool(pkt_allocator *allocator) {
	return (void*)((char*)allocator + pkt_AllocatorSize());
}

pkt_pool *pkt_AddPool(pkt_allocator *allocator, void *memory, pkt_size size) {
	pkt__lock_thread_access;

	//Offset it back by the pointer size, we don't need the prev_physical block pointer as there is none
	//for the first block in the pool
	pkt_header *block = pkt__first_block_in_pool(memory);
	block->size = 0;
	//Leave room for an end block
	pkt__set_block_size(block, size - (pkt__BLOCK_POINTER_OFFSET)-pkt__BLOCK_SIZE_OVERHEAD);

	//Make sure it aligns
	pkt__set_block_size(block, pkt__align_size_down(pkt__block_size(block), pkt__MEMORY_ALIGNMENT));
	PKT_ASSERT(pkt__block_size(block) > pkt__MINIMUM_BLOCK_SIZE);
	pkt__block_set_free(block);
	pkt__block_set_prev_used(block);

	//Add a 0 sized block at the end of the pool to cap it off
	pkt_header *last_block = pkt__next_physical_block(block);
	last_block->size = 0;
	pkt__block_set_used(last_block);

	last_block->prev_physical_block = block;
	pkt__push_block(allocator, block);

	pkt__unlock_thread_access;
	return memory;
}

pkt_bool pkt_RemovePool(pkt_allocator *allocator, pkt_pool *pool) {
	pkt__lock_thread_access;
	pkt_header *block = pkt__first_block_in_pool(pool);

	if (pkt__is_free_block(block) && !pkt__next_block_is_free(block) && pkt__is_last_block_in_pool(pkt__next_physical_block(block))) {
		pkt__remove_block_from_segregated_list(allocator, block);
		pkt__unlock_thread_access;
		return 1;
	}
#if defined(PKT_THREAD_SAFE)
	pkt__unlock_thread_access;
	PKT_PRINT_ERROR(PKT_ERROR_COLOR"%s: In order to remove a pool there must be only 1 free block in the pool. Was possibly freed by another thread\n", PKT_ERROR_NAME);
#else
	PKT_PRINT_ERROR(PKT_ERROR_COLOR"%s: In order to remove a pool there must be only 1 free block in the pool.\n", PKT_ERROR_NAME);
#endif
	return 0;
}

#if defined(PKT_ENABLE_REMOTE_MEMORY)
void *pkt__allocate(pkt_allocator *allocator, pkt_size size, pkt_size remote_size) {
#else
void *pkt_Allocate(pkt_allocator *allocator, pkt_size size) {
#endif
	pkt__lock_thread_access;
	pkt_index fli;
	pkt_index sli;
	size = pkt__adjust_size(size, pkt__MINIMUM_BLOCK_SIZE, pkt__MEMORY_ALIGNMENT);
	pkt__map(pkt__map_size, &fli, &sli);
	//Note that there may well be an appropriate size block in the class but that block may not be at the head of the list
	//In this situation we could opt to loop through the list of the size class to see if there is an appropriate size but instead
	//we stick to the paper and just move on to the next class up to keep a O1 speed at the cost of some extra fragmentation
	if (pkt__has_free_block(allocator, fli, sli) && pkt__block_size(allocator->segregated_lists[fli][sli]) >= pkt__map_size) {
		void *user_ptr = pkt__block_user_ptr(pkt__pop_block(allocator, fli, sli));
		pkt__unlock_thread_access;
		return user_ptr;
	}
	if (sli == pkt__SECOND_LEVEL_INDEX_COUNT - 1) {
		sli = -1;
	}
	else {
		sli = pkt__find_next_size_up(allocator->second_level_bitmaps[fli], sli);
	}
	if (sli == -1) {
		fli = pkt__find_next_size_up(allocator->first_level_bitmap, fli);
		if (fli > -1) {
			sli = pkt__scan_forward(allocator->second_level_bitmaps[fli]);
			pkt_header *block = pkt__pop_block(allocator, fli, sli);
			void *allocation = pkt__call_maybe_split_block;
			pkt__unlock_thread_access;
			return allocation;
		}
	}
	else {
		pkt_header *block = pkt__pop_block(allocator, fli, sli);
		void *allocation = pkt__call_maybe_split_block;
		pkt__unlock_thread_access;
		return allocation;
	}
	//Out of memory;
	PKT_PRINT_ERROR(PKT_ERROR_COLOR"%s: Not enough memory in pool to allocate %zu bytes\n", PKT_ERROR_NAME, pkt__map_size);
	pkt__unlock_thread_access;
	return 0;
}

void *pkt_Reallocate(pkt_allocator *allocator, void *ptr, pkt_size size) {
	pkt__lock_thread_access;

	if (ptr && size == 0) {
		pkt__unlock_thread_access;
		pkt_Free(allocator, ptr);
	}

	if (!ptr) {
		pkt__unlock_thread_access;
		return pkt_Allocate(allocator, size);
	}

	pkt_header *block = pkt__block_from_allocation(ptr);
	pkt_header *next_block = pkt__next_physical_block(block);
	void *allocation = 0;
	pkt_size current_size = pkt__block_size(block);
	pkt_size adjusted_size = pkt__adjust_size(size, allocator->minimum_allocation_size, pkt__MEMORY_ALIGNMENT);
	pkt_size combined_size = current_size + pkt__block_size(next_block);
	if ((!pkt__next_block_is_free(block) || adjusted_size > combined_size) && adjusted_size > current_size) {
		pkt__access_override;
		allocation = pkt_Allocate(allocator, size);
		if (allocation) {
			pkt_size smallest_size = pkt__Min(current_size, size);
			memcpy(allocation, ptr, smallest_size);
			pkt__do_unable_to_reallocate_callback;
			pkt_Free(allocator, ptr);
		}
	}
	else {
		//Reallocation is possible
		if (adjusted_size > current_size)
		{
			pkt__merge_with_next_block(allocator, block);
			pkt__mark_block_as_used(block);
		}
		allocation = pkt__maybe_split_block(allocator, block, adjusted_size, 0);
	}

	pkt__unlock_thread_access;
	return allocation;
}

int pkt_Free(pkt_allocator *allocator, void* allocation) {
	if (!allocation) return 0;
	pkt__lock_thread_access;
	pkt_header *block = pkt__block_from_allocation(allocation);
	if (pkt__prev_is_free_block(block)) {
		PKT_ASSERT(block->prev_physical_block);		//Must be a valid previous physical block
		block = pkt__merge_with_prev_block(allocator, block);
	}
	if (pkt__next_block_is_free(block)) {
		pkt__merge_with_next_block(allocator, block);
	}
	pkt__push_block(allocator, block);
	pkt__unlock_thread_access;
	return 1;
}

#if defined(PKT_ENABLE_REMOTE_MEMORY)
/*
	Standard callbacks, you can copy paste these to replace with your own as needed to add any extra functionality
	that you might need
*/
void pkt__remote_merge_next_callback(void *user_data, pkt_header *block, pkt_header *next_block) {
	pkt_remote_header *remote_block = (pkt_remote_header*)pkt_BlockUserExtensionPtr(block);
	pkt_remote_header *next_remote_block = (pkt_remote_header*)pkt_BlockUserExtensionPtr(next_block);
	remote_block->size += next_remote_block->size;
	next_remote_block->memory_offset = 0;
	next_remote_block->size = 0;
}

void pkt__remote_merge_prev_callback(void *user_data, pkt_header *prev_block, pkt_header *block) {
	pkt_remote_header *remote_block = (pkt_remote_header*)pkt_BlockUserExtensionPtr(block);
	pkt_remote_header *prev_remote_block = (pkt_remote_header*)pkt_BlockUserExtensionPtr(prev_block);
	prev_remote_block->size += remote_block->size;
	remote_block->memory_offset = 0;
	remote_block->size = 0;
}

pkt_size pkt__get_remote_size(const pkt_header *block) {
	pkt_remote_header *remote_block = (pkt_remote_header*)pkt_BlockUserExtensionPtr(block);
	return remote_block->size;
}

void *pkt_Allocate(pkt_allocator *allocator, pkt_size size) {
	return pkt__allocate(allocator, size, 0);
}

void pkt_SetBlockExtensionSize(pkt_allocator *allocator, pkt_size size) {
	PKT_ASSERT(allocator->block_extension_size == 0);	//You cannot change this once set
	allocator->block_extension_size = pkt__align_size_up(size, pkt__MEMORY_ALIGNMENT);
}

pkt_size pkt_CalculateRemoteBlockPoolSize(pkt_allocator *allocator, pkt_size remote_pool_size) {
	PKT_ASSERT(allocator->block_extension_size);	//You must set the block extension size first
	PKT_ASSERT(allocator->minimum_allocation_size);		//You must set the number of bytes per block
	return (sizeof(pkt_header) + allocator->block_extension_size) * (remote_pool_size / allocator->minimum_allocation_size) + pkt__BLOCK_POINTER_OFFSET;
}

void pkt_AddRemotePool(pkt_allocator *allocator, void *block_memory, pkt_size block_memory_size, pkt_size remote_pool_size) {
	PKT_ASSERT(allocator->add_pool_callback);	//You must set all the necessary callbacks to handle remote memory management
	PKT_ASSERT(allocator->get_block_size_callback);
	PKT_ASSERT(allocator->merge_next_callback);
	PKT_ASSERT(allocator->merge_prev_callback);
	PKT_ASSERT(allocator->split_block_callback);
	PKT_ASSERT(allocator->get_block_size_callback != pkt__block_size);	//Make sure you initialise the remote allocator with pkt_InitialiseAllocatorForRemote

	void *block = pkt_BlockUserExtensionPtr(pkt__first_block_in_pool(block_memory));
	pkt__do_add_pool_callback;
	pkt_AddPool(allocator, block_memory, block_memory_size);
}

pkt_allocator *pkt_InitialiseAllocatorForRemote(void *memory) {
	if (!memory) {
		PKT_PRINT_ERROR(PKT_ERROR_COLOR"%s: The memory pointer passed in to the initialiser was NULL, did it allocate properly?\n", PKT_ERROR_NAME);
		return 0;
	}

	pkt_allocator *allocator = pkt_InitialiseAllocator(memory);
	if (!allocator) {
		return 0;
	}

	allocator->get_block_size_callback = pkt__get_remote_size;
	allocator->merge_next_callback = pkt__remote_merge_next_callback;
	allocator->merge_prev_callback = pkt__remote_merge_prev_callback;

	return allocator;
}

void *pkt_AllocateRemote(pkt_allocator *allocator, pkt_size remote_size) {
	PKT_ASSERT(allocator->minimum_allocation_size > 0);
	remote_size = pkt__adjust_size(remote_size, allocator->minimum_allocation_size, pkt__MEMORY_ALIGNMENT);
	void* allocation = pkt__allocate(allocator, (remote_size / allocator->minimum_allocation_size) * (allocator->block_extension_size + pkt__BLOCK_POINTER_OFFSET), remote_size);
	return allocation ? (char*)allocation + pkt__MINIMUM_BLOCK_SIZE : 0;
}

void *pkt__reallocate_remote(pkt_allocator *allocator, void *ptr, pkt_size size, pkt_size remote_size) {
	pkt__lock_thread_access;

	if (ptr && remote_size == 0) {
		pkt__unlock_thread_access;
		pkt_FreeRemote(allocator, ptr);
	}

	if (!ptr) {
		pkt__unlock_thread_access;
		return pkt__allocate(allocator, size, remote_size);
	}

	pkt_header *block = pkt__block_from_allocation(ptr);
	pkt_header *next_block = pkt__next_physical_block(block);
	void *allocation = 0;
	pkt_size current_size = pkt__block_size(block);
	pkt_size current_remote_size = pkt__do_size_class_callback(block);
	pkt_size adjusted_size = pkt__adjust_size(size, allocator->minimum_allocation_size, pkt__MEMORY_ALIGNMENT);
	pkt_size combined_size = current_size + pkt__block_size(next_block);
	pkt_size combined_remote_size = current_remote_size + pkt__do_size_class_callback(next_block);
	if ((!pkt__next_block_is_free(block) || adjusted_size > combined_size || remote_size > combined_remote_size) && (remote_size > current_remote_size)) {
		pkt__access_override;
		allocation = pkt__allocate(allocator, size, remote_size);
		if (allocation) {
			pkt_remote_header *remote_block = (pkt_remote_header*)pkt_BlockUserExtensionPtr(block);
			pkt_size test_remote_size = pkt__do_size_class_callback(block);
			pkt__do_unable_to_reallocate_callback;
			pkt_Free(allocator, ptr);
		}
	}
	else {
		//Reallocation is possible
		if (remote_size > current_remote_size)
		{
			pkt__merge_with_next_block(allocator, block);
			pkt__mark_block_as_used(block);
		}
		allocation = pkt__maybe_split_block(allocator, block, adjusted_size, remote_size);
	}

	pkt__unlock_thread_access;
	return allocation;
}

void *pkt_ReallocateRemote(pkt_allocator *allocator, void *block_extension, pkt_size remote_size) {
	PKT_ASSERT(allocator->minimum_allocation_size > 0);
	void* allocation = pkt__reallocate_remote(allocator, block_extension ? pkt_AllocationFromExtensionPtr(block_extension) : block_extension, (remote_size / allocator->minimum_allocation_size) * (allocator->block_extension_size + pkt__BLOCK_POINTER_OFFSET), remote_size);
	return allocation ? (char*)allocation + pkt__MINIMUM_BLOCK_SIZE : 0;
}

int pkt_FreeRemote(pkt_allocator *allocator, void* block_extension) {
	void *allocation = (char*)block_extension - pkt__MINIMUM_BLOCK_SIZE;
	return pkt_Free(allocator, allocation);
}
#endif

#endif

/*
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2023 Peter Rigby
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/

