
/*	Pocket Allocator, a Two Level Segregated Fit memory allocator

	This software is dual-licensed. See bottom of file for license details.
*/

#ifndef ZLOC_INCLUDE_H
#define ZLOC_INCLUDE_H

//#define ZLOC_DEV_MODE

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>

//Header
#define zloc__Min(a, b) (((a) < (b)) ? (a) : (b))
#define zloc__Max(a, b) (((a) > (b)) ? (a) : (b))

#ifndef ZLOC_API
#define ZLOC_API
#endif

typedef int zloc_index;
typedef unsigned int zloc_sl_bitmap;
typedef unsigned int zloc_uint;
typedef unsigned int zloc_thread_access;
typedef int zloc_bool;
typedef void* zloc_pool;

#if !defined (ZLOC_ASSERT)
#define ZLOC_ASSERT assert
#endif

#define zloc__is_pow2(x) ((x) && !((x) & ((x) - 1)))
#define zloc__glue2(x, y) x ## y
#define zloc__glue(x, y) zloc__glue2(x, y)
#define zloc__static_assert(exp) \
	typedef char zloc__glue(static_assert, __LINE__) [(exp) ? 1 : -1]

#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__x86_64__)
#define zloc__64BIT
typedef size_t zloc_size;
typedef size_t zloc_fl_bitmap;
#define ZLOC_ONE 1ULL
#else
typedef size_t zloc_size;
typedef size_t zloc_fl_bitmap;
#define ZLOC_ONE 1U
#endif

#ifndef MEMORY_ALIGNMENT_LOG2
#if defined(zloc__64BIT)
#define MEMORY_ALIGNMENT_LOG2 3		//64 bit
#else
#define MEMORY_ALIGNMENT_LOG2 2		//32 bit
#endif
#endif

#ifndef ZLOC_ERROR_NAME
#define ZLOC_ERROR_NAME "Allocator Error"
#endif

#ifndef ZLOC_ERROR_COLOR
#define ZLOC_ERROR_COLOR "\033[31m"
#endif

#ifdef ZLOC_OUTPUT_ERROR_MESSAGES
#include <stdio.h>
#define ZLOC_PRINT_ERROR(message_f, ...) printf(message_f"\033[0m", __VA_ARGS__)
#else
#define ZLOC_PRINT_ERROR(message_f, ...)
#endif

#define zloc__KILOBYTE(Value) ((Value) * 1024LL)
#define zloc__MEGABYTE(Value) (zloc__KILOBYTE(Value) * 1024LL)
#define zloc__GIGABYTE(Value) (zloc__MEGABYTE(Value) * 1024LL)

#ifndef ZLOC_MAX_SIZE_INDEX
#if defined(zloc__64BIT)
#define ZLOC_MAX_SIZE_INDEX 32
#else
#define ZLOC_MAX_SIZE_INDEX 30
#endif
#endif

zloc__static_assert(ZLOC_MAX_SIZE_INDEX < 64);

#ifdef __cplusplus
extern "C" {
#endif

#define zloc__MAXIMUM_BLOCK_SIZE (ZLOC_ONE << ZLOC_MAX_SIZE_INDEX)

	enum zloc__constants {
		zloc__MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
		zloc__SECOND_LEVEL_INDEX_LOG2 = 5,
		zloc__FIRST_LEVEL_INDEX_COUNT = ZLOC_MAX_SIZE_INDEX,
		zloc__SECOND_LEVEL_INDEX_COUNT = 1 << zloc__SECOND_LEVEL_INDEX_LOG2,
		zloc__BLOCK_POINTER_OFFSET = sizeof(void*) + sizeof(zloc_size),
		zloc__MINIMUM_BLOCK_SIZE = 16,
		zloc__BLOCK_SIZE_OVERHEAD = sizeof(zloc_size),
		zloc__POINTER_SIZE = sizeof(void*),
        zloc__SMALLEST_CATEGORY = (1 << (zloc__SECOND_LEVEL_INDEX_LOG2 + MEMORY_ALIGNMENT_LOG2))
	};

	typedef enum zloc__boundary_tag_flags {
		zloc__BLOCK_IS_FREE = 1 << 0,
		zloc__PREV_BLOCK_IS_FREE = 1 << 1,
	} zloc__boundary_tag_flags;

	typedef enum zloc__error_codes {
		zloc__OK,
		zloc__INVALID_FIRST_BLOCK,
		zloc__INVALID_BLOCK_FOUND,
		zloc__PHYSICAL_BLOCK_MISALIGNMENT,
		zloc__INVALID_SEGRATED_LIST,
		zloc__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST,
		zloc__SECOND_LEVEL_BITMAPS_NOT_INITIALISED
	} zloc__error_codes;

	typedef enum zloc__thread_ops {
		zloc__FREEING_BLOCK = 1 << 0,
		zloc__ALLOCATING_BLOCK = 1 << 1
	} zloc__thread_ops;

	/*
		Each block has a header that if used only has a pointer to the previous physical block
		and the size. If the block is free then the prev and next free blocks are also stored.
	*/
	typedef struct zloc_header {
		struct zloc_header *prev_physical_block;
		/*	Note that the size is either 4 or 8 bytes aligned so the boundary tag (2 flags denoting
			whether this or the previous block is free) can be stored in the first 2 least
			significant bits	*/
		zloc_size size;
		/*
		User allocation will start here when the block is used. When the block is free prev and next
		are pointers in a linked list of free blocks within the same class size of blocks
		*/
		struct zloc_header *prev_free_block;
		struct zloc_header *next_free_block;
	} zloc_header;

	typedef struct zloc_allocator {
		/*	This is basically a terminator block that free blocks can point to if they're at the end
			of a free list. */
		zloc_header null_block;
#if defined(ZLOC_THREAD_SAFE)
		/* Multithreading protection*/
		volatile zloc_thread_access access;
		volatile zloc_thread_access access_override;
#endif
#if defined(ZLOC_ENABLE_REMOTE_MEMORY)
		void *user_data;
		zloc_size(*get_block_size_callback)(const zloc_header* block);
		void(*merge_next_callback)(void *user_data, zloc_header* block, zloc_header *next_block);
		void(*merge_prev_callback)(void *user_data, zloc_header* prev_block, zloc_header *block);
		void(*split_block_callback)(void *user_data, zloc_header* block, zloc_header* trimmed_block, zloc_size remote_size);
		void(*add_pool_callback)(void *user_data, void* block_extension);
		void(*unable_to_reallocate_callback)(void *user_data, zloc_header *block, zloc_header *new_block);
		zloc_size block_extension_size;
#endif
		zloc_size minimum_allocation_size;
		/*	Here we store all of the free block data. first_level_bitmap is either a 32bit int
		or 64bit depending on whether zloc__64BIT is set. Second_level_bitmaps are an array of 32bit
		ints. segregated_lists is a two level array pointing to free blocks or null_block if the list
		is empty. */
		zloc_fl_bitmap first_level_bitmap;
		zloc_sl_bitmap second_level_bitmaps[zloc__FIRST_LEVEL_INDEX_COUNT];
		zloc_header *segregated_lists[zloc__FIRST_LEVEL_INDEX_COUNT][zloc__SECOND_LEVEL_INDEX_COUNT];
	} zloc_allocator;

#if defined(ZLOC_ENABLE_REMOTE_MEMORY)
	/*
	A minimal remote header block. You can define your own header to store additional information but it must include
	size and memory_offset in the first 2 fields.
	*/
	typedef struct zloc_remote_header {
		zloc_size size;
		zloc_size memory_offset;
	} zloc_remote_header;

#define zloc__map_size (remote_size ? remote_size : size)
#define zloc__do_size_class_callback(block) allocator->get_block_size_callback(block)
#define zloc__do_merge_next_callback allocator->merge_next_callback(allocator->user_data, block, next_block)
#define zloc__do_merge_prev_callback allocator->merge_prev_callback(allocator->user_data, prev_block, block)
#define zloc__do_split_block_callback allocator->split_block_callback(allocator->user_data, block, trimmed, remote_size)
#define zloc__do_add_pool_callback allocator->add_pool_callback(allocator->user_data, block)
#define zloc__do_unable_to_reallocate_callback zloc_header *new_block = zloc__block_from_allocation(allocation); zloc_header *block = zloc__block_from_allocation(ptr); allocator->unable_to_reallocate_callback(allocator->user_data, block, new_block)
#define zloc__block_extension_size (allocator->block_extension_size & ~1)
#define zloc__call_maybe_split_block zloc__maybe_split_block(allocator, block, size, remote_size) 
#else
#define zloc__map_size size
#define zloc__do_size_class_callback(block) zloc__block_size(block)
#define zloc__do_merge_next_callback
#define zloc__do_merge_prev_callback
#define zloc__do_split_block_callback
#define zloc__do_add_pool_callback
#define zloc__do_unable_to_reallocate_callback
#define zloc__block_extension_size 0
#define zloc__call_maybe_split_block zloc__maybe_split_block(allocator, block, size, 0) 
#endif

#if defined (_MSC_VER) && (_MSC_VER >= 1400) && (defined (_M_IX86) || defined (_M_X64))
	/* Microsoft Visual C++ support on x86/X64 architectures. */

#include <intrin.h>

	static inline int zloc__scan_reverse(zloc_size bitmap) {
		unsigned long index;
#if defined(zloc__64BIT)
		return _BitScanReverse64(&index, bitmap) ? index : -1;
#else
		return _BitScanReverse(&index, bitmap) ? index : -1;
#endif
	}

	static inline int zloc__scan_forward(zloc_size bitmap)
	{
		unsigned long index;
#if defined(zloc__64BIT)
		return _BitScanForward64(&index, bitmap) ? index : -1;
#else
		return _BitScanForward(&index, bitmap) ? index : -1;
#endif
	}

#ifdef _WIN32
#include <Windows.h>
	static inline zloc_thread_access zloc__compare_and_exchange(volatile zloc_thread_access* target, zloc_thread_access value, zloc_thread_access original) {
		return InterlockedCompareExchange(target, value, original);
	}
#endif

#elif defined(__GNUC__) && ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)) && \
      (defined(__i386__) || defined(__x86_64__)) || defined(__clang__)
	/* GNU C/C++ or Clang support on x86/x64 architectures. */

	static inline int zloc__scan_reverse(zloc_size bitmap)
	{
#if defined(zloc__64BIT)
		return 64 - __builtin_clzll(bitmap) - 1;
#else
		return 32 - __builtin_clz((int)bitmap) - 1;
#endif
	}

	static inline int zloc__scan_forward(zloc_size bitmap)
	{
#if defined(zloc__64BIT)
		return __builtin_ffsll(bitmap) - 1;
#else
		return __builtin_ffs((int)bitmap) - 1;
#endif
	}

	static inline zloc_thread_access zloc__compare_and_exchange(volatile zloc_thread_access* target, zloc_thread_access value, zloc_thread_access original) {
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
		@param	zloc_size				The size of the memory you're passing
		@returns zloc_allocator*			A pointer to a zloc_allocator which you'll need to use when calling zloc_Allocate or zloc_Free. Note that
										this pointer will be the same address as the memory you're passing in as all the information the allocator
										stores to organise memory blocks is stored at the beginning of the memory.
										If something went wrong then 0 is returned. Define ZLOC_OUTPUT_ERROR_MESSAGES before including this header
										file to see any errors in the console.
	*/
	ZLOC_API zloc_allocator *zloc_InitialiseAllocator(void *memory);

	/*
		Initialise an allocator and a pool at the same time. The data stucture to store the allocator will be stored at the beginning of the memory
		you pass to the function and the remaining memory will be used as the pool.

		@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
		@param	zloc_size				The size of the memory you're passing
		@returns zloc_allocator*		A pointer to a zloc_allocator which you'll need to use when calling zloc_Allocate or zloc_Free.
										If something went wrong then 0 is returned. Define ZLOC_OUTPUT_ERROR_MESSAGES before including this header
										file to see any errors in the console.
	*/
	ZLOC_API zloc_allocator *zloc_InitialiseAllocatorWithPool(void *memory, zloc_size size);

	/*
		Add a new memory pool to the allocator. Pools don't have to all be the same size, adding a pool will create the biggest block it can within
		the pool and then add that to the segregated list of free blocks in the allocator. All the pools in the allocator will be naturally linked
		together in the segregated list because all blocks are linked together with a linked list either as physical neighbours or free blocks in
		the segregated list.

		@param	zloc_allocator*			A pointer to some previously initialised allocator
		@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
		@param	zloc_size				The size of the memory you're passing
		@returns zloc_pool*				A pointer to the pool
	*/
	ZLOC_API zloc_pool *zloc_AddPool(zloc_allocator *allocator, void *memory, zloc_size size);

	/*
		Get the structure size of an allocator. You can use this to take into account the overhead of the allocator when preparing a new allocator
		with memory pool.

		@returns zloc_size				The struct size of the allocator in bytes
	*/
	ZLOC_API zloc_size zloc_AllocatorSize(void);

	/*
		If you initialised an allocator with a pool then you can use this function to get a pointer to the start of the pool. It won't get a pointer
		to any other pool in the allocator. You can just get that when you call zloc_AddPool.

		@param	zloc_allocator*			A pointer to some previously initialised allocator
		@returns zloc_pool				A pointer to the pool memory in the allocator
	*/
	ZLOC_API zloc_pool *zloc_GetPool(zloc_allocator *allocator);

	/*
		Allocate some memory within a zloc_allocator of the specified size. Minimum size is 16 bytes.

		@param	zloc_allocator			A pointer to an initialised zloc_allocator
		@param	zloc_size				The size of the memory you're passing
		@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to
										no free memory. If that happens then you may want to add a pool at that point.
	*/
	ZLOC_API void *zloc_Allocate(zloc_allocator *allocator, zloc_size size);

	/*
		Try to reallocate an existing memory block within the allocator. If possible the current block will be merged with the physical neigbouring
		block, otherwise a normal zloc_Allocate will take place and the data copied over to the new allocation.

		@param	zloc_size				The size of the memory you're passing
		@param	void*					A ptr to the current memory you're reallocating
		@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to
										no free memory. If that happens then you may want to add a pool at that point.
	*/
	ZLOC_API void *zloc_Reallocate(zloc_allocator *allocator, void *ptr, zloc_size size);

	/*
	Allocate some memory within a zloc_allocator of the specified size. Minimum size is 16 bytes.

	@param	zloc_allocator			A pointer to an initialised zloc_allocator
	@param	zloc_size				The size of the memory you're passing
	@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to
									no free memory. If that happens then you may want to add a pool at that point.
*/
	ZLOC_API void *zloc_AllocateAligned(zloc_allocator *allocator, zloc_size size, zloc_size alignment);

	/*
		Free an allocation from a zloc_allocator. When freeing a block of memory any adjacent free blocks are merged together to keep on top of
		fragmentation as much as possible. A check is also done to confirm that the block being freed is still valid and detect any memory corruption
		due to out of bounds writing of this or potentially other blocks.

		It's recommended to call this function with an assert: ZLOC_ASSERT(zloc_Free(allocator, allocation));
		An error is also output to console as long as ZLOC_OUTPUT_ERROR_MESSAGES is defined.

		@returns int		returns 1 if the allocation was successfully freed, 0 otherwise.
	*/
	ZLOC_API int zloc_Free(zloc_allocator *allocator, void *allocation);

	/*
		Remove a pool from an allocator. Note that all blocks in the pool must be free and therefore all merged together into one block (this happens
		automatically as all blocks are freed are merged together into bigger blocks.

		@param zloc_allocator*			A pointer to a tcoc_allocator that you want to reset
		@param zloc_allocator*			A pointer to the memory pool that you want to free. You get this pointer when you add a pool to the allocator.
	*/
	ZLOC_API zloc_bool zloc_RemovePool(zloc_allocator *allocator, zloc_pool *pool);

	/*
	When using an allocator for managing remote memory, you need to set the bytes per block that a block storing infomation about the remote
	memory allocation will manage. For example you might set the value to 1MB so if you were to then allocate 4MB of remote memory then 4 blocks
	worth of space would be used to allocate that memory. This means that if it were to be freed and then split down to a smaller size they'd be
	enough blocks worth of space to do this.

	Note that the lower the number the more memory you need to track remote memory blocks but the more granular it will be. It will depend alot
	on the size of allocations you will need

	@param zloc_allocator*			A pointer to an initialised allocator
	@param zloc_size				The bytes per block you want it to be set to. Must be a power of 2
*/
	ZLOC_API void zloc_SetMinimumAllocationSize(zloc_allocator *allocator, zloc_size size);

#if defined(ZLOC_ENABLE_REMOTE_MEMORY)
	/*
		Initialise an allocator and a pool at the same time and flag it for use as a remote memory manager.
		The data stucture to store the allocator will be stored at the beginning of the memory you pass to the function and the remaining memory will
		be used as the pool. Use with zloc_CalculateRemoteBlockPoolSize to allow for the number of memory ranges you might need to manage in the
		remote memory pool(s)

		@param	void*					A pointer to some previously allocated memory that was created with malloc, VirtualAlloc etc.
		@param	zloc_size				The size of the memory you're passing
		@returns zloc_allocator*		A pointer to a zloc_allocator which you'll need to use when calling zloc_AllocateRemote or zloc_FreeRemote.
										If something went wrong then 0 is returned. Define ZLOC_OUTPUT_ERROR_MESSAGES before including this header
										file to see any errors in the console.
	*/
	ZLOC_API zloc_allocator *zloc_InitialiseAllocatorForRemote(void *memory);

	/*
		When using an allocator for managing remote memory, you need to set the size of the struct that you will be using to store information about
		the remote block of memory. This will be like an extension the existing zloc_header.

		@param zloc_allocator*			A pointer to an initialised allocator
		@param zloc_size				The size of the block extension. Will be aligned up to zloc__MEMORY_ALIGNMENT
	*/
	ZLOC_API void zloc_SetBlockExtensionSize(zloc_allocator *allocator, zloc_size size);

	/*
		Free a remote allocation from a zloc_allocator. You must have set up merging callbacks so that you can update your block extensions with the
		necessary buffer sizes and offsets

		It's recommended to call this function with an assert: ZLOC_ASSERT(zloc_FreeRemote(allocator, allocation));
		An error is also output to console as long as ZLOC_OUTPUT_ERROR_MESSAGES is defined.

		@returns int		returns 1 if the allocation was successfully freed, 0 otherwise.
	*/
	ZLOC_API int zloc_FreeRemote(zloc_allocator *allocator, void *allocation);

	/*
		Allocate some memory in a remote location from the normal heap. This is generally for allocating GPU memory.

		@param	zloc_allocator			A pointer to an initialised zloc_allocator
		@param	zloc_size				The size of the memory you're passing which should be the size of the block with the information about the
										buffer you're creating in the remote location
		@param	zloc_size				The remote size of the memory you're passing
		@returns void*					A pointer to the block of memory that is allocated. Returns 0 if it was unable to allocate the memory due to
										no free memory. If that happens then you may want to add a pool at that point.
	*/
	ZLOC_API void *zloc_AllocateRemote(zloc_allocator *allocator, zloc_size remote_size);

	/*
		Get the size of a block plus the block extension size so that you can use this to create an allocator pool to store all the blocks that will
		track the remote memory. Be sure that you have already called and set the block extension size with zloc_SetBlockExtensionSize.

		@param	zloc_allocator			A pointer to an initialised zloc_allocator
		@returns zloc_size				The size of the block
	*/
	ZLOC_API zloc_size zloc_CalculateRemoteBlockPoolSize(zloc_allocator *allocator, zloc_size remote_pool_size);

	ZLOC_API void zloc_AddRemotePool(zloc_allocator *allocator, void *block_memory, zloc_size block_memory_size, zloc_size remote_pool_size);

    ZLOC_API void* zloc_BlockUserExtensionPtr(const zloc_header *block);

    ZLOC_API void* zloc_AllocationFromExtensionPtr(const void *block);

#endif

	//--End of user functions

	//Private inline functions, user doesn't need to call these

	static inline void zloc__map(zloc_size size, zloc_index *fli, zloc_index *sli) {
		*fli = zloc__scan_reverse(size);
        if(*fli <= zloc__SECOND_LEVEL_INDEX_LOG2) {
            *fli = 0;
            *sli = (int)size / (zloc__SMALLEST_CATEGORY / zloc__SECOND_LEVEL_INDEX_COUNT);
            return;
        }
		size = size & ~(1 << *fli);
		*sli = (zloc_index)(size >> (*fli - zloc__SECOND_LEVEL_INDEX_LOG2)) % zloc__SECOND_LEVEL_INDEX_COUNT;
	}

#if defined(ZLOC_ENABLE_REMOTE_MEMORY)
	static inline void zloc__null_merge_callback(void *user_data, zloc_header *block1, zloc_header *block2) { return; }
	void zloc__remote_merge_next_callback(void *user_data, zloc_header *block1, zloc_header *block2);
	void zloc__remote_merge_prev_callback(void *user_data, zloc_header *block1, zloc_header *block2);
	zloc_size zloc__get_remote_size(const zloc_header *block1);
	static inline void zloc__null_split_callback(void *user_data, zloc_header *block, zloc_header *trimmed, zloc_size remote_size) { return; }
	static inline void zloc__null_add_pool_callback(void *user_data, void *block) { return; }
	static inline void zloc__null_unable_to_reallocate_callback(void *user_data, zloc_header *block, zloc_header *new_block) { return; }
	static inline void zloc__unset_remote_block_limit_reached(zloc_allocator *allocator) { allocator->block_extension_size &= ~1; };
#endif

	//Read only functions
	static inline zloc_bool zloc__has_free_block(const zloc_allocator *allocator, zloc_index fli, zloc_index sli) {
		return allocator->first_level_bitmap & (ZLOC_ONE << fli) && allocator->second_level_bitmaps[fli] & (1U << sli);
	}

	static inline zloc_bool zloc__is_used_block(const zloc_header *block) {
		return !(block->size & zloc__BLOCK_IS_FREE);
	}

	static inline zloc_bool zloc__is_free_block(const zloc_header *block) {
		return block->size & zloc__BLOCK_IS_FREE;
	}

	static inline zloc_bool zloc__prev_is_free_block(const zloc_header *block) {
		return block->size & zloc__PREV_BLOCK_IS_FREE;
	}

	static inline void* zloc__align_ptr(const void* ptr, zloc_size align) {
		ptrdiff_t aligned = (((ptrdiff_t)ptr) + (align - 1)) & ~(align - 1);
		ZLOC_ASSERT(0 == (align & (align - 1)) && "must align to a power of two");
		return (void*)aligned;
	}

	static inline zloc_bool zloc__is_aligned(zloc_size size, zloc_size alignment) {
		return (size % alignment) == 0;
	}

	static inline zloc_bool zloc__ptr_is_aligned(void *ptr, zloc_size alignment) {
		uintptr_t address = (uintptr_t)ptr;
		return (address % alignment) == 0;
	}

	static inline zloc_size zloc__align_size_down(zloc_size size, zloc_size alignment) {
		return size - (size % alignment);
	}

	static inline zloc_size zloc__align_size_up(zloc_size size, zloc_size alignment) {
		zloc_size remainder = size % alignment;
		if (remainder != 0) {
			size += alignment - remainder;
		}
		return size;
	}

	static inline zloc_size zloc__adjust_size(zloc_size size, zloc_size minimum_size, zloc_size alignment) {
		return zloc__Min(zloc__Max(zloc__align_size_up(size, alignment), minimum_size), zloc__MAXIMUM_BLOCK_SIZE);
	}

	static inline zloc_size zloc__block_size(const zloc_header *block) {
		return block->size & ~(zloc__BLOCK_IS_FREE | zloc__PREV_BLOCK_IS_FREE);
	}

	static inline zloc_header *zloc__block_from_allocation(const void *allocation) {
		return (zloc_header*)((char*)allocation - zloc__BLOCK_POINTER_OFFSET);
	}

	static inline zloc_header *zloc__null_block(zloc_allocator *allocator) {
		return &allocator->null_block;
	}

	static inline void* zloc__block_user_ptr(const zloc_header *block) {
		return (char*)block + zloc__BLOCK_POINTER_OFFSET;
	}

	static inline zloc_header* zloc__first_block_in_pool(const zloc_pool *pool) {
		return (zloc_header*)((char*)pool - zloc__POINTER_SIZE);
	}

	static inline zloc_header *zloc__next_physical_block(const zloc_header *block) {
		return (zloc_header*)((char*)zloc__block_user_ptr(block) + zloc__block_size(block));
	}

	static inline zloc_bool zloc__next_block_is_free(const zloc_header *block) {
		return zloc__is_free_block(zloc__next_physical_block(block));
	}

	static inline zloc_header *zloc__allocator_first_block(zloc_allocator *allocator) {
		return (zloc_header*)((char*)allocator + zloc_AllocatorSize() - zloc__POINTER_SIZE);
	}

	static inline zloc_bool zloc__is_last_block_in_pool(const zloc_header *block) {
		return zloc__block_size(block) == 0;
	}

	static inline zloc_index zloc__find_next_size_up(zloc_fl_bitmap map, zloc_uint start) {
		//Mask out all bits up to the start point of the scan
		map &= (~0ULL << (start + 1));
		return zloc__scan_forward(map);
	}

	//Write functions
#if defined(ZLOC_THREAD_SAFE)

#define zloc__lock_thread_access												\
	if (allocator->access + allocator->access_override != 2) {					\
		do {																	\
		} while (0 != zloc__compare_and_exchange(&allocator->access, 1, 0));	\
	}																			\
	ZLOC_ASSERT(allocator->access != 0);

#define zloc__unlock_thread_access allocator->access_override = 0; allocator->access = 0;
#define zloc__access_override allocator->access_override = 1;

#else

#define zloc__lock_thread_access
#define zloc__unlock_thread_access 
#define zloc__access_override

#endif
	void *zloc__allocate(zloc_allocator *allocator, zloc_size size, zloc_size remote_size);

	static inline void zloc__set_block_size(zloc_header *block, zloc_size size) {
		zloc_size boundary_tag = block->size & (zloc__BLOCK_IS_FREE | zloc__PREV_BLOCK_IS_FREE);
		block->size = size | boundary_tag;
	}

	static inline void zloc__set_prev_physical_block(zloc_header *block, zloc_header *prev_block) {
		block->prev_physical_block = prev_block;
	}

	static inline void zloc__zero_block(zloc_header *block) {
		block->prev_physical_block = 0;
		block->size = 0;
	}

	static inline void zloc__mark_block_as_used(zloc_header *block) {
		block->size &= ~zloc__BLOCK_IS_FREE;
		zloc_header *next_block = zloc__next_physical_block(block);
		next_block->size &= ~zloc__PREV_BLOCK_IS_FREE;
	}

	static inline void zloc__mark_block_as_free(zloc_header *block) {
		block->size |= zloc__BLOCK_IS_FREE;
		zloc_header *next_block = zloc__next_physical_block(block);
		next_block->size |= zloc__PREV_BLOCK_IS_FREE;
	}

	static inline void zloc__block_set_used(zloc_header *block) {
		block->size &= ~zloc__BLOCK_IS_FREE;
	}

	static inline void zloc__block_set_free(zloc_header *block) {
		block->size |= zloc__BLOCK_IS_FREE;
	}

	static inline void zloc__block_set_prev_used(zloc_header *block) {
		block->size &= ~zloc__PREV_BLOCK_IS_FREE;
	}

	static inline void zloc__block_set_prev_free(zloc_header *block) {
		block->size |= zloc__PREV_BLOCK_IS_FREE;
	}

	/*
		Push a block onto the segregated list of free blocks. Called when zloc_Free is called. Generally blocks are
		merged if possible before this is called
	*/
	static inline void zloc__push_block(zloc_allocator *allocator, zloc_header *block) {
		zloc_index fli;
		zloc_index sli;
		//Get the size class of the block
		zloc__map(zloc__do_size_class_callback(block), &fli, &sli);
		zloc_header *current_block_in_free_list = allocator->segregated_lists[fli][sli];
		//Insert the block into the list by updating the next and prev free blocks of
		//this and the current block in the free list. The current block in the free
		//list may well be the null_block in the allocator so this just means that this
		//block will be added as the first block in this class of free blocks.
		block->next_free_block = current_block_in_free_list;
		block->prev_free_block = &allocator->null_block;
		current_block_in_free_list->prev_free_block = block;

		allocator->segregated_lists[fli][sli] = block;
		//Flag the bitmaps to mark that this size class now contains a free block
		allocator->first_level_bitmap |= ZLOC_ONE << fli;
		allocator->second_level_bitmaps[fli] |= 1U << sli;
		zloc__mark_block_as_free(block);
	}

	/*
		Remove a block from the segregated list in the allocator and return it. If there is a next free block in the size class
		then move it down the list, otherwise unflag the bitmaps as necessary. This is only called when we're trying to allocate
		some memory with zloc_Allocate and we've determined that there's a suitable free block in segregated_lists.
	*/
	static inline zloc_header *zloc__pop_block(zloc_allocator *allocator, zloc_index fli, zloc_index sli) {
		zloc_header *block = allocator->segregated_lists[fli][sli];

		//If the block in the segregated list is actually the null_block then something went very wrong.
		//Somehow the segregated lists had the end block assigned but the first or second level bitmaps
		//did not have the masks assigned
		ZLOC_ASSERT(block != &allocator->null_block);
		if (block->next_free_block != &allocator->null_block) {
			//If there are more free blocks in this size class then shift the next one down and terminate the prev_free_block
			allocator->segregated_lists[fli][sli] = block->next_free_block;
			allocator->segregated_lists[fli][sli]->prev_free_block = zloc__null_block(allocator);
		}
		else {
			//There's no more free blocks in this size class so flag the second level bitmap for this class to 0.
			allocator->segregated_lists[fli][sli] = zloc__null_block(allocator);
			allocator->second_level_bitmaps[fli] &= ~(1U << sli);
			if (allocator->second_level_bitmaps[fli] == 0) {
				//And if the second level bitmap is 0 then the corresponding bit in the first lebel can be zero'd too.
				allocator->first_level_bitmap &= ~(ZLOC_ONE << fli);
			}
		}
		zloc__mark_block_as_used(block);
		return block;
	}

	/*
		Remove a block from the segregated list. This is only called when we're merging blocks together. The block is
		just removed from the list and marked as used and then merged with an adjacent block.
	*/
	static inline void zloc__remove_block_from_segregated_list(zloc_allocator *allocator, zloc_header *block) {
		zloc_index fli, sli;
		//Get the size class
		zloc__map(zloc__do_size_class_callback(block), &fli, &sli);
		zloc_header *prev_block = block->prev_free_block;
		zloc_header *next_block = block->next_free_block;
		ZLOC_ASSERT(prev_block);
		ZLOC_ASSERT(next_block);
		next_block->prev_free_block = prev_block;
		prev_block->next_free_block = next_block;
		if (allocator->segregated_lists[fli][sli] == block) {
			allocator->segregated_lists[fli][sli] = next_block;
			if (next_block == zloc__null_block(allocator)) {
				allocator->second_level_bitmaps[fli] &= ~(1U << sli);
				if (allocator->second_level_bitmaps[fli] == 0) {
					allocator->first_level_bitmap &= ~(1ULL << fli);
				}
			}
		}
		zloc__mark_block_as_used(block);
	}

	/*
		This function is called when zloc_Allocate is called. Once a free block is found then it will be split
		if the size + header overhead + the minimum block size (16b) is greater then the size of the free block.
		If not then it simply returns the free block as it is without splitting.
		If split then the trimmed amount is added back to the segregated list of free blocks.
	*/
	static inline zloc_header *zloc__maybe_split_block(zloc_allocator *allocator, zloc_header *block, zloc_size size, zloc_size remote_size) {
		ZLOC_ASSERT(!zloc__is_last_block_in_pool(block));
		zloc_size size_plus_overhead = size + zloc__BLOCK_POINTER_OFFSET + zloc__block_extension_size;
		if (size_plus_overhead + zloc__MINIMUM_BLOCK_SIZE >= zloc__block_size(block) - zloc__block_extension_size) {
			return block;
		}
		zloc_header *trimmed = (zloc_header*)((char*)zloc__block_user_ptr(block) + size + zloc__block_extension_size);
		trimmed->size = 0;
		zloc__set_block_size(trimmed, zloc__block_size(block) - size_plus_overhead);
		zloc_header *next_block = zloc__next_physical_block(block);
		zloc__set_prev_physical_block(next_block, trimmed);
		zloc__set_prev_physical_block(trimmed, block);
		zloc__set_block_size(block, size + zloc__block_extension_size);
		zloc__do_split_block_callback;
		zloc__push_block(allocator, trimmed);
		return block;
	}

	//For splitting blocks when allocating to a specific memory alignment
	static inline zloc_header *zloc__split_aligned_block(zloc_allocator *allocator, zloc_header *block, zloc_size size) {
		ZLOC_ASSERT(!zloc__is_last_block_in_pool(block));
		zloc_size size_minus_overhead = size - zloc__BLOCK_POINTER_OFFSET;
		zloc_header *trimmed = (zloc_header*)((char*)zloc__block_user_ptr(block) + size_minus_overhead);
		trimmed->size = 0;
		zloc__set_block_size(trimmed, zloc__block_size(block) - size);
		zloc_header *next_block = zloc__next_physical_block(block);
		zloc__set_prev_physical_block(next_block, trimmed);
		zloc__set_prev_physical_block(trimmed, block);
		zloc__set_block_size(block, size_minus_overhead);
		zloc__push_block(allocator, block);
		return trimmed;
	}

	/*
		This function is called when zloc_Free is called and the previous physical block is free. If that's the case
		then this function will merge the block being freed with the previous physical block then add that back into
		the segregated list of free blocks. Note that that happens in the zloc_Free function after attempting to merge
		both ways.
	*/
	static inline zloc_header *zloc__merge_with_prev_block(zloc_allocator *allocator, zloc_header *block) {
		ZLOC_ASSERT(!zloc__is_last_block_in_pool(block));
		zloc_header *prev_block = block->prev_physical_block;
		zloc__remove_block_from_segregated_list(allocator, prev_block);
		zloc__do_merge_prev_callback;
		zloc__set_block_size(prev_block, zloc__block_size(prev_block) + zloc__block_size(block) + zloc__BLOCK_POINTER_OFFSET);
		zloc_header *next_block = zloc__next_physical_block(block);
		zloc__set_prev_physical_block(next_block, prev_block);
		zloc__zero_block(block);
		return prev_block;
	}

	/*
		This function might be called when zloc_Free is called to free a block. If the block being freed is not the last
		physical block then this function is called and if the next block is free then it will be merged.
	*/
	static inline void zloc__merge_with_next_block(zloc_allocator *allocator, zloc_header *block) {
		zloc_header *next_block = zloc__next_physical_block(block);
		ZLOC_ASSERT(next_block->prev_physical_block == block);	//could be potentional memory corruption. Check that you're not writing outside the boundary of the block size
		ZLOC_ASSERT(!zloc__is_last_block_in_pool(next_block));
		zloc__remove_block_from_segregated_list(allocator, next_block);
		zloc__do_merge_next_callback;
		zloc__set_block_size(block, zloc__block_size(next_block) + zloc__block_size(block) + zloc__BLOCK_POINTER_OFFSET);
		zloc_header *block_after_next = zloc__next_physical_block(next_block);
		zloc__set_prev_physical_block(block_after_next, block);
		zloc__zero_block(next_block);
	}

	static inline zloc_header *zloc__find_free_block(zloc_allocator *allocator, zloc_size size, zloc_size remote_size) {
		zloc_index fli;
		zloc_index sli;
		zloc__map(zloc__map_size, &fli, &sli);
		//Note that there may well be an appropriate size block in the class but that block may not be at the head of the list
		//In this situation we could opt to loop through the list of the size class to see if there is an appropriate size but instead
		//we stick to the paper and just move on to the next class up to keep a O1 speed at the cost of some extra fragmentation
		if (zloc__has_free_block(allocator, fli, sli) && zloc__block_size(allocator->segregated_lists[fli][sli]) >= zloc__map_size) {
			zloc_header *block = zloc__pop_block(allocator, fli, sli);
			zloc__unlock_thread_access;
			return block;
		}
		if (sli == zloc__SECOND_LEVEL_INDEX_COUNT - 1) {
			sli = -1;
		}
		else {
			sli = zloc__find_next_size_up(allocator->second_level_bitmaps[fli], sli);
		}
		if (sli == -1) {
			fli = zloc__find_next_size_up(allocator->first_level_bitmap, fli);
			if (fli > -1) {
				sli = zloc__scan_forward(allocator->second_level_bitmaps[fli]);
				zloc_header *block = zloc__pop_block(allocator, fli, sli);
				zloc_header *split_block = zloc__call_maybe_split_block;
				zloc__unlock_thread_access;
				return split_block;
			}
		}
		else {
			zloc_header *block = zloc__pop_block(allocator, fli, sli);
			zloc_header *split_block = zloc__call_maybe_split_block;
			zloc__unlock_thread_access;
			return split_block;
		}

		return 0;
	}
	//--End of internal functions

	//--End of header declarations

#ifdef __cplusplus
}
#endif

#endif

//Implementation
#if defined(ZLOC_IMPLEMENTATION) || defined(ZLOC_DEV_MODE)

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

//Definitions
ZLOC_API void* zloc_BlockUserExtensionPtr(const zloc_header *block) {
    return (char*)block + sizeof(zloc_header);
}

ZLOC_API void* zloc_AllocationFromExtensionPtr(const void *block) {
    return (void*)((char*)block - zloc__MINIMUM_BLOCK_SIZE);
}

zloc_allocator *zloc_InitialiseAllocator(void *memory) {
	if (!memory) {
		ZLOC_PRINT_ERROR(ZLOC_ERROR_COLOR"%s: The memory pointer passed in to the initialiser was NULL, did it allocate properly?\n", ZLOC_ERROR_NAME);
		return 0;
	}

	zloc_allocator *allocator = (zloc_allocator*)memory;
	memset(allocator, 0, sizeof(zloc_allocator));
	allocator->null_block.next_free_block = &allocator->null_block;
	allocator->null_block.prev_free_block = &allocator->null_block;
	allocator->minimum_allocation_size = zloc__MINIMUM_BLOCK_SIZE;

	//Point all of the segregated list array pointers to the empty block
	for (zloc_uint i = 0; i < zloc__FIRST_LEVEL_INDEX_COUNT; i++) {
		for (zloc_uint j = 0; j < zloc__SECOND_LEVEL_INDEX_COUNT; j++) {
			allocator->segregated_lists[i][j] = &allocator->null_block;
		}
	}

#if defined(ZLOC_ENABLE_REMOTE_MEMORY)
	allocator->get_block_size_callback = zloc__block_size;
	allocator->merge_next_callback = zloc__null_merge_callback;
	allocator->merge_prev_callback = zloc__null_merge_callback;
	allocator->split_block_callback = zloc__null_split_callback;
	allocator->add_pool_callback = zloc__null_add_pool_callback;
	allocator->unable_to_reallocate_callback = zloc__null_unable_to_reallocate_callback;
#endif

	return allocator;
}

zloc_allocator *zloc_InitialiseAllocatorWithPool(void *memory, zloc_size size) {
	zloc_size array_offset = sizeof(zloc_allocator);
	if (size < array_offset + zloc__MEMORY_ALIGNMENT) {
		ZLOC_PRINT_ERROR(ZLOC_ERROR_COLOR"%s: Tried to initialise allocator with a memory allocation that is too small. Must be at least: %zi bytes\n", ZLOC_ERROR_NAME, array_offset + zloc__MEMORY_ALIGNMENT);
		return 0;
	}

	zloc_allocator *allocator = zloc_InitialiseAllocator(memory);
	if (!allocator) {
		return 0;
	}
	zloc_AddPool(allocator, zloc_GetPool(allocator), size - zloc_AllocatorSize());
	return allocator;
}

zloc_size zloc_AllocatorSize(void) {
	return sizeof(zloc_allocator);
}

void zloc_SetMinimumAllocationSize(zloc_allocator *allocator, zloc_size size) {
	ZLOC_ASSERT(allocator->minimum_allocation_size == zloc__MINIMUM_BLOCK_SIZE);		//You cannot change this once set
	ZLOC_ASSERT(zloc__is_pow2(size));													//Size must be a power of 2
	allocator->minimum_allocation_size = zloc__Max(zloc__MINIMUM_BLOCK_SIZE, size);
}

zloc_pool *zloc_GetPool(zloc_allocator *allocator) {
	return (void*)((char*)allocator + zloc_AllocatorSize());
}

zloc_pool *zloc_AddPool(zloc_allocator *allocator, void *memory, zloc_size size) {
	zloc__lock_thread_access;

	//Offset it back by the pointer size, we don't need the prev_physical block pointer as there is none
	//for the first block in the pool
	zloc_header *block = zloc__first_block_in_pool(memory);
	block->size = 0;
	//Leave room for an end block
	zloc__set_block_size(block, size - (zloc__BLOCK_POINTER_OFFSET)-zloc__BLOCK_SIZE_OVERHEAD);

	//Make sure it aligns
	zloc__set_block_size(block, zloc__align_size_down(zloc__block_size(block), zloc__MEMORY_ALIGNMENT));
	ZLOC_ASSERT(zloc__block_size(block) > zloc__MINIMUM_BLOCK_SIZE);
	zloc__block_set_free(block);
	zloc__block_set_prev_used(block);

	//Add a 0 sized block at the end of the pool to cap it off
	zloc_header *last_block = zloc__next_physical_block(block);
	last_block->size = 0;
	zloc__block_set_used(last_block);

	last_block->prev_physical_block = block;
	zloc__push_block(allocator, block);

	zloc__unlock_thread_access;
	return memory;
}

zloc_bool zloc_RemovePool(zloc_allocator *allocator, zloc_pool *pool) {
	zloc__lock_thread_access;
	zloc_header *block = zloc__first_block_in_pool(pool);

	if (zloc__is_free_block(block) && !zloc__next_block_is_free(block) && zloc__is_last_block_in_pool(zloc__next_physical_block(block))) {
		zloc__remove_block_from_segregated_list(allocator, block);
		zloc__unlock_thread_access;
		return 1;
	}
#if defined(ZLOC_THREAD_SAFE)
	zloc__unlock_thread_access;
	ZLOC_PRINT_ERROR(ZLOC_ERROR_COLOR"%s: In order to remove a pool there must be only 1 free block in the pool. Was possibly freed by another thread\n", ZLOC_ERROR_NAME);
#else
	ZLOC_PRINT_ERROR(ZLOC_ERROR_COLOR"%s: In order to remove a pool there must be only 1 free block in the pool.\n", ZLOC_ERROR_NAME);
#endif
	return 0;
}

#if defined(ZLOC_ENABLE_REMOTE_MEMORY)
void *zloc__allocate(zloc_allocator *allocator, zloc_size size, zloc_size remote_size) {
#else
void *zloc_Allocate(zloc_allocator *allocator, zloc_size size) {
	zloc_size remote_size = 0;
#endif
	zloc__lock_thread_access;
	size = zloc__adjust_size(size, zloc__MINIMUM_BLOCK_SIZE, zloc__MEMORY_ALIGNMENT);
	zloc_header *block = zloc__find_free_block(allocator, size, remote_size);

	if (block) {
		return zloc__block_user_ptr(block);
	}

	//Out of memory;
	ZLOC_PRINT_ERROR(ZLOC_ERROR_COLOR"%s: Not enough memory in pool to allocate %zu bytes\n", ZLOC_ERROR_NAME, zloc__map_size);
	zloc__unlock_thread_access;
	return 0;
}

void *zloc_Reallocate(zloc_allocator *allocator, void *ptr, zloc_size size) {
	zloc__lock_thread_access;

	if (ptr && size == 0) {
		zloc__unlock_thread_access;
		zloc_Free(allocator, ptr);
	}

	if (!ptr) {
		zloc__unlock_thread_access;
		return zloc_Allocate(allocator, size);
	}

	zloc_header *block = zloc__block_from_allocation(ptr);
	zloc_header *next_block = zloc__next_physical_block(block);
	void *allocation = 0;
	zloc_size current_size = zloc__block_size(block);
	zloc_size adjusted_size = zloc__adjust_size(size, allocator->minimum_allocation_size, zloc__MEMORY_ALIGNMENT);
	zloc_size combined_size = current_size + zloc__block_size(next_block);
	if ((!zloc__next_block_is_free(block) || adjusted_size > combined_size) && adjusted_size > current_size) {
		zloc__access_override;
		allocation = zloc_Allocate(allocator, size);
		if (allocation) {
			zloc_size smallest_size = zloc__Min(current_size, size);
			memcpy(allocation, ptr, smallest_size);
			zloc__do_unable_to_reallocate_callback;
			zloc_Free(allocator, ptr);
		}
	}
	else {
		//Reallocation is possible
		if (adjusted_size > current_size)
		{
			zloc__merge_with_next_block(allocator, block);
			zloc__mark_block_as_used(block);
		}
		zloc_header *split_block = zloc__maybe_split_block(allocator, block, adjusted_size, 0);
		allocation = zloc__block_user_ptr(split_block);
	}

	zloc__unlock_thread_access;
	return allocation;
}

void *zloc_AllocateAligned(zloc_allocator *allocator, zloc_size size, zloc_size alignment) {
	zloc_size adjusted_size = zloc__adjust_size(size, allocator->minimum_allocation_size, alignment);
	zloc_size gap_minimum = sizeof(zloc_header);
	zloc_size size_with_gap = zloc__adjust_size(adjusted_size + alignment + gap_minimum, allocator->minimum_allocation_size, alignment);
	size_t aligned_size = (adjusted_size && alignment > zloc__MEMORY_ALIGNMENT) ? size_with_gap : adjusted_size;

	zloc_header *block = zloc__find_free_block(allocator, aligned_size, 0);

	if (block) {
		void *user_ptr = zloc__block_user_ptr(block);
		void *aligned_ptr = zloc__align_ptr(user_ptr, alignment);
		zloc_size gap = (zloc_size)(((ptrdiff_t)aligned_ptr) - (ptrdiff_t)user_ptr);

		/* If gap size is too small, offset to next aligned boundary. */
		if (gap && gap < gap_minimum)
		{
			zloc_size gap_remain = gap_minimum - gap;
			zloc_size offset = zloc__Max(gap_remain, alignment);
			const void* next_aligned = (void*)((ptrdiff_t)aligned_ptr + offset);

			aligned_ptr = zloc__align_ptr(next_aligned, alignment);
			gap = (zloc_size)((ptrdiff_t)aligned_ptr - (ptrdiff_t)user_ptr);
		}

		if (gap)
		{
			ZLOC_ASSERT(gap >= gap_minimum && "gap size too small");
			block = zloc__split_aligned_block(allocator, block, gap);
			zloc__block_set_used(block);
		}
		ZLOC_ASSERT(zloc__ptr_is_aligned(zloc__block_user_ptr(block), alignment));	//pointer not aligned to requested alignment
	}
	else {
		return 0;
	}

	return zloc__block_user_ptr(block);
}

int zloc_Free(zloc_allocator *allocator, void* allocation) {
	if (!allocation) return 0;
	zloc__lock_thread_access;
	zloc_header *block = zloc__block_from_allocation(allocation);
	if (zloc__prev_is_free_block(block)) {
		ZLOC_ASSERT(block->prev_physical_block);		//Must be a valid previous physical block
		block = zloc__merge_with_prev_block(allocator, block);
	}
	if (zloc__next_block_is_free(block)) {
		zloc__merge_with_next_block(allocator, block);
	}
	zloc__push_block(allocator, block);
	zloc__unlock_thread_access;
	return 1;
}

#if defined(ZLOC_ENABLE_REMOTE_MEMORY)
/*
	Standard callbacks, you can copy paste these to replace with your own as needed to add any extra functionality
	that you might need
*/
void zloc__remote_merge_next_callback(void *user_data, zloc_header *block, zloc_header *next_block) {
	zloc_remote_header *remote_block = (zloc_remote_header*)zloc_BlockUserExtensionPtr(block);
	zloc_remote_header *next_remote_block = (zloc_remote_header*)zloc_BlockUserExtensionPtr(next_block);
	remote_block->size += next_remote_block->size;
	next_remote_block->memory_offset = 0;
	next_remote_block->size = 0;
}

void zloc__remote_merge_prev_callback(void *user_data, zloc_header *prev_block, zloc_header *block) {
	zloc_remote_header *remote_block = (zloc_remote_header*)zloc_BlockUserExtensionPtr(block);
	zloc_remote_header *prev_remote_block = (zloc_remote_header*)zloc_BlockUserExtensionPtr(prev_block);
	prev_remote_block->size += remote_block->size;
	remote_block->memory_offset = 0;
	remote_block->size = 0;
}

zloc_size zloc__get_remote_size(const zloc_header *block) {
	zloc_remote_header *remote_block = (zloc_remote_header*)zloc_BlockUserExtensionPtr(block);
	return remote_block->size;
}

void *zloc_Allocate(zloc_allocator *allocator, zloc_size size) {
	return zloc__allocate(allocator, size, 0);
}

void zloc_SetBlockExtensionSize(zloc_allocator *allocator, zloc_size size) {
	ZLOC_ASSERT(allocator->block_extension_size == 0);	//You cannot change this once set
	allocator->block_extension_size = zloc__align_size_up(size, zloc__MEMORY_ALIGNMENT);
}

zloc_size zloc_CalculateRemoteBlockPoolSize(zloc_allocator *allocator, zloc_size remote_pool_size) {
	ZLOC_ASSERT(allocator->block_extension_size);	//You must set the block extension size first
	ZLOC_ASSERT(allocator->minimum_allocation_size);		//You must set the number of bytes per block
	return (sizeof(zloc_header) + allocator->block_extension_size) * (remote_pool_size / allocator->minimum_allocation_size) + zloc__BLOCK_POINTER_OFFSET;
}

void zloc_AddRemotePool(zloc_allocator *allocator, void *block_memory, zloc_size block_memory_size, zloc_size remote_pool_size) {
	ZLOC_ASSERT(allocator->add_pool_callback);	//You must set all the necessary callbacks to handle remote memory management
	ZLOC_ASSERT(allocator->get_block_size_callback);
	ZLOC_ASSERT(allocator->merge_next_callback);
	ZLOC_ASSERT(allocator->merge_prev_callback);
	ZLOC_ASSERT(allocator->split_block_callback);
	ZLOC_ASSERT(allocator->get_block_size_callback != zloc__block_size);	//Make sure you initialise the remote allocator with zloc_InitialiseAllocatorForRemote

	void *block = zloc_BlockUserExtensionPtr(zloc__first_block_in_pool(block_memory));
	zloc__do_add_pool_callback;
	zloc_AddPool(allocator, block_memory, block_memory_size);
}

zloc_allocator *zloc_InitialiseAllocatorForRemote(void *memory) {
	if (!memory) {
		ZLOC_PRINT_ERROR(ZLOC_ERROR_COLOR"%s: The memory pointer passed in to the initialiser was NULL, did it allocate properly?\n", ZLOC_ERROR_NAME);
		return 0;
	}

	zloc_allocator *allocator = zloc_InitialiseAllocator(memory);
	if (!allocator) {
		return 0;
	}

	allocator->get_block_size_callback = zloc__get_remote_size;
	allocator->merge_next_callback = zloc__remote_merge_next_callback;
	allocator->merge_prev_callback = zloc__remote_merge_prev_callback;

	return allocator;
}

void *zloc_AllocateRemote(zloc_allocator *allocator, zloc_size remote_size) {
	ZLOC_ASSERT(allocator->minimum_allocation_size > 0);
	remote_size = zloc__adjust_size(remote_size, allocator->minimum_allocation_size, zloc__MEMORY_ALIGNMENT);
	void* allocation = zloc__allocate(allocator, (remote_size / allocator->minimum_allocation_size) * (allocator->block_extension_size + zloc__BLOCK_POINTER_OFFSET), remote_size);
	return allocation ? (char*)allocation + zloc__MINIMUM_BLOCK_SIZE : 0;
}

void *zloc__reallocate_remote(zloc_allocator *allocator, void *ptr, zloc_size size, zloc_size remote_size) {
	zloc__lock_thread_access;

	if (ptr && remote_size == 0) {
		zloc__unlock_thread_access;
		zloc_FreeRemote(allocator, ptr);
	}

	if (!ptr) {
		zloc__unlock_thread_access;
		return zloc__allocate(allocator, size, remote_size);
	}

	zloc_header *block = zloc__block_from_allocation(ptr);
	zloc_header *next_block = zloc__next_physical_block(block);
	void *allocation = 0;
	zloc_size current_size = zloc__block_size(block);
	zloc_size current_remote_size = zloc__do_size_class_callback(block);
	zloc_size adjusted_size = zloc__adjust_size(size, allocator->minimum_allocation_size, zloc__MEMORY_ALIGNMENT);
	zloc_size combined_size = current_size + zloc__block_size(next_block);
	zloc_size combined_remote_size = current_remote_size + zloc__do_size_class_callback(next_block);
	if ((!zloc__next_block_is_free(block) || adjusted_size > combined_size || remote_size > combined_remote_size) && (remote_size > current_remote_size)) {
		zloc__access_override;
		allocation = zloc__allocate(allocator, size, remote_size);
		if (allocation) {
			zloc__do_unable_to_reallocate_callback;
			zloc_Free(allocator, ptr);
		}
	}
	else {
		//Reallocation is possible
		if (remote_size > current_remote_size)
		{
			zloc__merge_with_next_block(allocator, block);
			zloc__mark_block_as_used(block);
		}
		zloc_header *split_block = zloc__maybe_split_block(allocator, block, adjusted_size, remote_size);
		allocation = zloc__block_user_ptr(split_block);
	}

	zloc__unlock_thread_access;
	return allocation;
}

void *zloc_ReallocateRemote(zloc_allocator *allocator, void *block_extension, zloc_size remote_size) {
	ZLOC_ASSERT(allocator->minimum_allocation_size > 0);
	void* allocation = zloc__reallocate_remote(allocator, block_extension ? zloc_AllocationFromExtensionPtr(block_extension) : block_extension, (remote_size / allocator->minimum_allocation_size) * (allocator->block_extension_size + zloc__BLOCK_POINTER_OFFSET), remote_size);
	return allocation ? (char*)allocation + zloc__MINIMUM_BLOCK_SIZE : 0;
}

int zloc_FreeRemote(zloc_allocator *allocator, void* block_extension) {
	void *allocation = (char*)block_extension - zloc__MINIMUM_BLOCK_SIZE;
	return zloc_Free(allocator, allocation);
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

