#include <stdio.h>
#include <time.h>

//#define TLOC_DEV_MODE

#if !defined(TLOC_DEV_MODE)
#define TLOC_ERROR_COLOR "\033[90m"
#define TLOC_IMPLEMENTATION
//#define TLOC_OUTPUT_ERROR_MESSAGES
#define TLOC_THREAD_SAFE
#define TLOC_MAX_SIZE_INDEX 35		//max block size 34GB
#include "2loc.h"
#define _TIMESPEC_DEFINED
#ifdef _WIN32
#ifdef TLOC_THREAD_SAFE	
#include <pthread.h>
typedef void *(PTW32_CDECL *tloc__allocation_thread)(void*);
#endif
#include <windows.h>
#define tloc_sleep(seconds) Sleep(seconds)
#else
#ifdef TLOC_THREAD_SAFE	
#include <pthread.h>
typedef void *(PTW32_CDECL *tloc__allocation_thread)(void*);
#endif
#include <unistd.h>
#define tloc_sleep(seconds) sleep(seconds)
#endif

#define tloc_free_memory(memory) if(memory) free(memory);
#define TWO63 0x8000000000000000u 
#define TWO64f (TWO63*2.0)

//Debugging and validation
typedef void(*tloc__block_output)(void* ptr, size_t size, int used, void* user, int is_final_output);

typedef struct tloc_random {
	unsigned long long seeds[2];
} tloc_random;

void _Advance(tloc_random *random) {
	unsigned long long s1 = random->seeds[0];
	unsigned long long s0 = random->seeds[1];
	random->seeds[0] = s0;
	s1 ^= s1 << 23; // a
	random->seeds[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
}

void _ReSeed(tloc_random *random, unsigned long long seed) {
	random->seeds[0] = seed;
	random->seeds[1] = seed * 2;
	_Advance(random);
}

double _Generate(tloc_random *random) {
	unsigned long long s1 = random->seeds[0];
	unsigned long long s0 = random->seeds[1];
	unsigned long long result = s0 + s1;
	random->seeds[0] = s0;
	s1 ^= s1 << 23; // a
	random->seeds[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
	double test = TWO64f;
	return (double)result / TWO64f;
}

unsigned long long _tloc_random_range(tloc_random *random, unsigned long long max) {
	double g = _Generate(random);
	double a = g * (double)max;
	return (unsigned long long)a;
};

void PrintTestResult(const char *message, int result) {
	printf("%s", message);
	printf("%s [%s]\033[0m\n", result == 0 ? "\033[31m" : "\033[32m", result == 0 ? "FAILED" : "PASSED");
}

static void tloc__output(void* ptr, size_t size, int free, void* user, int is_final_output)
{
	(void)user;
	tloc_header *block = (tloc_header*)ptr;
	printf("\t%p %s size: %zi (%p), (%p), (%p)\n", ptr, free ? "free" : "used", size, ptr, block->next_free_block, block->prev_free_block);
	if (is_final_output) {
		printf("\t------------- * ---------------\n");
	}
}
//Some helper functions for debugging
//Makes sure that all blocks in the segregated list of free blocks are all valid
tloc__error_codes tloc_VerifySegregatedLists(tloc_allocator *allocator) {
	for (int fli = 0; fli != tloc__FIRST_LEVEL_INDEX_COUNT; ++fli) {
		for (int sli = 0; sli != tloc__SECOND_LEVEL_INDEX_COUNT; ++sli) {
			tloc_header *block = allocator->segregated_lists[fli][sli];
			if (block->size) {
				tloc_index size_fli, size_sli;
				tloc__map(tloc__block_size(block), &size_fli, &size_sli);
				if (size_fli != fli && size_sli != sli) {
					return tloc__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST;
				}
			}
			if (block == tloc__end(allocator)) {
				continue;
			}
		}
	}
	return tloc__OK;
}

//The segregated list of free blocks should never contain any null blocks, this seeks them out.
tloc_bool tloc_CheckForNullBlocksInList(tloc_allocator *allocator) {
	for (int fli = 0; fli != tloc__FIRST_LEVEL_INDEX_COUNT; ++fli) {
		if (allocator->first_level_bitmap & (1ULL << fli)) {
			for (int sli = 0; sli != tloc__SECOND_LEVEL_INDEX_COUNT; ++sli) {
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

//Search the segregated list of free blocks for a given block
tloc_bool tloc_BlockExistsInSegregatedList(tloc_allocator *allocator, tloc_header* block) {
	for (int fli = 0; fli != tloc__FIRST_LEVEL_INDEX_COUNT; ++fli) {
		for (int sli = 0; sli != tloc__SECOND_LEVEL_INDEX_COUNT; ++sli) {
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

//Loops through all blocks in the allocator and confirms that they all correctly link together
tloc__error_codes tloc_VerifyBlocks(tloc_allocator *allocator, tloc__block_output output_function, void *user_data) {
	tloc_header *current_block = tloc__allocator_first_block(allocator);
	while (!tloc__is_last_block_in_pool(current_block)) {
		if (output_function) {
			tloc__output(current_block, tloc__block_size(current_block), tloc__is_free_block(current_block), user_data, 0);
		}
		tloc_header *last_block = current_block;
		current_block = tloc__next_physical_block(current_block);
		if (last_block != current_block->prev_physical_block) {
			return tloc__PHYSICAL_BLOCK_MISALIGNMENT;
		}
	}
	if (output_function) {
		tloc__output(current_block, tloc__block_size(current_block), tloc__is_free_block(current_block), user_data, 1);
	}
	return tloc__OK;
}

tloc_header *tloc_SearchList(tloc_allocator *allocator, tloc_header *search) {
	tloc_header *current_block = tloc__allocator_first_block(allocator);
	if (search == current_block) {
		return current_block;
	}
	while (!tloc__is_last_block_in_pool(current_block)) {
		if (search == current_block) {
			return current_block;
		}
		current_block = tloc__next_physical_block(current_block);
	}
	return current_block == search ? current_block : 0;
}

//Test if passing a memory pool that is too small to the initialiser is handled gracefully
int TestPoolTooSmall() {
	void *memory = malloc(1024);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, 1024);
	if (!allocator) {
		tloc_free_memory(memory);
		return 1;
	}
	tloc_free_memory(memory);
	return 0;
}

//Test if trying to free an invalid memory block fails gracefully
int TestFreeingAnInvalidAllocation(void) {
	int result = 0;
	void *memory = malloc(tloc__MEGABYTE(1));
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, tloc__MEGABYTE(1));
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (allocator) {
		void *allocation = malloc(tloc__KILOBYTE(1));
		if (!tloc_Free(allocator, allocation)) {
			result = 1;
		}
		tloc_free_memory(allocation);
	}
	tloc_free_memory(memory);
	return result;
}

//Write outside the bounds of an allocation
int TestMemoryCorruptionDetection(void) {
	int result = 0;
	void *memory = malloc(tloc__MEGABYTE(1));
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, tloc__MEGABYTE(1));
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (allocator) {
		int *allocation = tloc_Allocate(allocator, sizeof(int) * 10);
		for (int i = 0; i != 20; ++i) {
			allocation[i] = rand();
		}
		if (!tloc_Free(allocator, allocation)) {
			result = 1;
		}
	}
	tloc_free_memory(memory);
	return result;
}

//Write outside the bounds of an allocation
int TestMemoryCorruptionDetection2(void) {
	int result = 0;
	void *memory = malloc(tloc__MEGABYTE(1));
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, tloc__MEGABYTE(1));
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (allocator) {
		int *allocation = tloc_Allocate(allocator, sizeof(int) * 10);
		for (int i = -5; i != 10; ++i) {
			allocation[i] = rand();
		}
		if (!tloc_Free(allocator, allocation)) {
			result = 1;
		}
	}
	tloc_free_memory(memory);
	return result;
}

int TestNonAlignedMemoryPool(void) {
	void *memory = malloc(1023);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, 1023);
	if (!allocator) {
		tloc_free_memory(memory);
		return 1;
	}
	tloc_free_memory(memory);
	return 0;
}

int TestAllocateSingleOverAllocate(void) {
	tloc_size size = tloc__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = tloc_Allocate(allocator, size);
		if (allocation) {
			result = 0;
		}
	}
	tloc_free_memory(memory);
	return result;
}

int TestAllocateMultiOverAllocate(void) {
	tloc_size size = tloc__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
	}
	tloc_size allocated = 0;
	while (tloc_Allocate(allocator, 1024)) {
		allocated += 1024;
		if (tloc_VerifyBlocks(allocator, 0, 0) != tloc__OK) {
			result = 0;
			break;
		}
		if (allocated > size) {
			result = 0;
			break;
		}
	}
	tloc_free_memory(memory);
	return result;
}

//This tests that free blocks in the segregated list will be exhausted first before using the main pool for allocations
int TestAllocateFreeSameSizeBlocks(void) {
	tloc_size size = tloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
	}
	tloc_size allocated = 0;
	void *allocations[40];
	for (int i = 0; i != 20; ++i) {
		allocations[i] = tloc_Allocate(allocator, 1024);
		if (!allocations[i]) {
			result = 0;
		}
	}
	//Free every second one so that blocks don't get merged
	for (int i = 0; i != 20; i += 2) {
		tloc_Free(allocator, allocations[i]);
		tloc__error_codes error = tloc_VerifyBlocks(allocator, 0, 0);
		assert(error == tloc__OK);
		allocations[i] = 0;
	}
	for (int i = 0; i != 40; ++i) {
		allocations[i] = tloc_Allocate(allocator, 1024);
		tloc__error_codes error = tloc_VerifyBlocks(allocator, 0, 0);
		assert(error == tloc__OK);
		if (!allocations[i]) {
			result = 0;
		}
	}
	for (int i = 0; i != 40; ++i) {
		if (allocations[i]) {
			tloc__error_codes error = tloc_VerifyBlocks(allocator, 0, 0);
			assert(error == tloc__OK);
			if (i == 11) {
				int d = 0;
			}
			tloc_Free(allocator, allocations[i]);
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
	}
	tloc_free_memory(memory);
	return result;
}

//Test allocating some memory that is too small
int TestAllocationTooSmall(void) {
	tloc_size size = tloc__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = tloc_Allocate(allocator, 4);
		if (tloc__block_size(tloc__block_from_allocation(allocation)) == tloc__MINIMUM_BLOCK_SIZE) {
			result = 1;
		}
		else {
			result = 0;
		}
	}
	tloc_free_memory(memory);
	return result;
}

int TestReAllocation(void) {
	tloc_size size = tloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = tloc_Allocate(allocator, 1024);
		if (!allocation) {
			result = 0;
		}
		else {
			allocation = tloc_Reallocate(allocator, allocation, 2048);
			if (!allocation) {
				result = 0;
			}
			else if (tloc__block_size(tloc__block_from_allocation(allocation)) != 2048) {
				result = 0;
			}
		}
	}
	tloc_free_memory(memory);
	return result;
}

int TestReAllocationFallbackToAllocateAndCopy(void) {
	tloc_size size = tloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation1 = tloc_Allocate(allocator, 1024);
		void *allocation2 = tloc_Allocate(allocator, 1024);
		allocation1 = tloc_Reallocate(allocator, allocation1, 2048);
		if (!allocation1) {
			result = 0;
		}
	}
	tloc_free_memory(memory);
	return result;
}

int TestReAllocationOfNullPtr(void) {
	tloc_size size = tloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = 0;
		allocation = tloc_Reallocate(allocator, allocation, 1024);
		if (!allocation) {
			result = 0;
		}
		else {
			result = 1;
		}
	}
	tloc_free_memory(memory);
	return result;
}

int TestManyAllocationsAndFreesDummy(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size) {
	int result = 1;
	tloc_size allocations[100];
	memset(allocations, 0, sizeof(tloc_size) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		if (allocations[index]) {
			allocations[index] = 0;
		}
		else {
			tloc_size allocation_size = (rand() % max_allocation_size) + tloc__MINIMUM_BLOCK_SIZE;
			allocations[index] = allocation_size;
		}
	}
	return result;
}

//Do lots of allocations and frees of random sizes
int TestManyAllocationsAndFrees(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size, tloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		for (int i = 0; i != iterations; ++i) {
			if (i == 12) {
				int d = 0;
			}
			int index = rand() % 100;
			if (allocations[index]) {
				tloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				tloc_size allocation_size = (tloc_size)_tloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//tloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				allocations[index] = tloc_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		tloc_free_memory(memory);
	}
	return result;
}

int TestManyAllocationsAndFreesAddPools(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size, tloc_random *random) {
	int result = 1;
	void *memory[8];
	int memory_index = 0;
	memory[memory_index] = malloc(pool_size);
	memset(memory[memory_index], 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory[memory_index++], pool_size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		for (int i = 0; i != iterations; ++i) {
			if (i == 12) {
				int d = 0;
			}
			int index = rand() % 100;
			if (allocations[index]) {
				tloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				tloc_size allocation_size = (tloc_size)_tloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//tloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				allocations[index] = tloc_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
				else if(memory_index < 8) {
					//We ran out of memory, add a new pool
					memory[memory_index] = malloc(pool_size);
					tloc_AddPool(allocator, memory[memory_index++], pool_size);
					allocations[index] = tloc_Allocate(allocator, allocation_size);
					if (!allocations[index]) {
						result = 0;
						break;
					}
				}
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		for (int i = 0; i != memory_index; ++i) {
			tloc_free_memory(memory[i]);
		}
	}
	return result;
}

int TestAllocatingUntilOutOfSpaceThenRandomFreesAndAllocations(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size, tloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			tloc_size allocation_size = (tloc_size)_tloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = tloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 1000;
			if (allocations[index]) {
				tloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				tloc_size allocation_size = (tloc_size)_tloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				allocations[index] = tloc_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		tloc_free_memory(memory);
	}
	return result;
}

//Test that after allocating lots of allocations and freeing them all, there should only be one free allocation
//because all free blocks should get merged
int TestAllocatingUntilOutOfSpaceThenFreeAll(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size, tloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			tloc_size allocation_size = (tloc_size)_tloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = tloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				tloc_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		tloc_header *first_block = tloc__allocator_first_block(allocator);
		result = tloc__is_last_block_in_pool(tloc__next_physical_block(first_block));
		tloc_free_memory(memory);
	}
	return result;
}

int TestRemovingPool(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size, tloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			tloc_size allocation_size = (tloc_size)_tloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = tloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				tloc_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		if (!tloc_RemovePool(allocator, tloc_GetPool(allocator))) {
			result = 0;
		}
		tloc_free_memory(memory);
	}
	return result;
}

int TestRemovingExtraPool(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size, tloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	tloc_pool *extra_pool = 0;
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			tloc_size allocation_size = (tloc_size)_tloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = tloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				void *extra_memory = malloc(pool_size);
				extra_pool = tloc_AddPool(allocator, extra_memory, pool_size);
				allocations[i] = tloc_Allocate(allocator, allocation_size);
				break;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				tloc_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		if (!tloc_RemovePool(allocator, extra_pool)) {
			result = 0;
		}
		if (!tloc_RemovePool(allocator, tloc_GetPool(allocator))) {
			result = 0;
		}
		tloc_free_memory(memory);
		if (extra_pool) {
			tloc_free_memory(extra_pool);
		}
	}
	return result;
}

//64bit tests
#if defined(tloc__64BIT)
//Allocate a large block
int TestAllocation64bit(void) {
	tloc_size size = (1024ull * 1024ull * 1024ull * 6ull);	//6 gb
	int result = 1;
	void* memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(memory, size);
	assert(tloc_VerifySegregatedLists(allocator) == tloc__OK);
	if (!allocator) {
		result = 0;
	}
	if(allocator) {
		if (tloc_Allocate(allocator, size / 2)) {
			result = 1;
		}
	}
	tloc_free_memory(memory);
	return result;
}
#endif

typedef struct tloc_thread_test {
	tloc_allocator *allocator;
	void *allocations[100];
	tloc_random *random;
	tloc_uint iterations;
	tloc_size pool_size;
	tloc_size min_allocation_size;
	tloc_size max_allocation_size;
	tloc_index memory_index;
} tloc_thread_test;

#ifdef TLOC_THREAD_SAFE
struct tloc_memory_threads {
	void *memory[9];
	volatile tloc_thread_access access;
};

struct tloc_memory_threads thread_memory;

// Function that will be executed by the thread
void *AllocationWorker(void *arg) {
	tloc_thread_test *thread_test = (tloc_thread_test*)arg;
	for (int i = 0; i != thread_test->iterations; ++i) {
		int index = rand() % 100;
		if (thread_test->allocations[index]) {
			tloc_Free(thread_test->allocator, thread_test->allocations[index]);
			thread_test->allocations[index] = 0;
		}
		else {
			tloc_size allocation_size = (tloc_size)_tloc_random_range(thread_test->random, thread_test->max_allocation_size - thread_test->min_allocation_size) + thread_test->min_allocation_size;
			thread_test->allocations[index] = tloc_Allocate(thread_test->allocator, allocation_size);
			if (thread_test->allocations[index]) {
				//Do a memset set to test if we're overwriting block headers
				memset(thread_test->allocations[index], 7, allocation_size);
			}
		}
	}
	return 0;
}

void *AllocationWorkerAddPool(void *arg) {
	tloc_thread_test *thread_test = (tloc_thread_test*)arg;
	for (int i = 0; i != thread_test->iterations; ++i) {
		int index = rand() % 100;
		if (thread_test->allocations[index]) {
			tloc_Free(thread_test->allocator, thread_test->allocations[index]);
			thread_test->allocations[index] = 0;
		}
		else {
			tloc_size allocation_size = (tloc_size)_tloc_random_range(thread_test->random, thread_test->max_allocation_size - thread_test->min_allocation_size) + thread_test->min_allocation_size;
			thread_test->allocations[index] = tloc_Allocate(thread_test->allocator, allocation_size);
			if (thread_test->allocations[index]) {
				//Do a memset set to test if we're overwriting block headers
				memset(thread_test->allocations[index], 7, allocation_size);
			}
			else if (thread_memory.memory[thread_test->memory_index] == 0) {
				//We ran out of memory, add a new pool
				tloc_thread_access original_access = thread_memory.access;
				tloc_thread_access access = tloc__compare_and_exchange(&thread_memory.access, 1, original_access);
				if (original_access == access) {
					tloc_index index = thread_test->memory_index;
					thread_memory.memory[index] = malloc(thread_test->pool_size);
					tloc_AddPool(thread_test->allocator, thread_memory.memory[index], thread_test->pool_size);
					printf("\033[34mThread %i added pool\033[0m\n", index);
					thread_test->allocations[index] = tloc_Allocate(thread_test->allocator, allocation_size);
					thread_memory.access = 0;
				}
				else {
					do {} while (thread_memory.access == 1);
					thread_test->allocations[index] = tloc_Allocate(thread_test->allocator, allocation_size);
				}
			}
		}
	}
	return 0;
}

int TestMultithreading(tloc__allocation_thread callback, tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size, int thread_count, tloc_random *random) {
	int result = 1;

	thread_memory.memory[0] = malloc(pool_size);
	tloc_thread_test thread[8];
	tloc_allocator *allocator = tloc_InitialiseAllocatorWithPool(thread_memory.memory[0], pool_size);
	if (!allocator) {
		return 0;
	}

	for (int i = 0; i != 9; ++i) {
		thread_memory.memory[i] = 0;
	}

	for (int i = 0; i != thread_count; ++i) {
		memset(thread[i].allocations, 0, sizeof(void*) * 100);
		thread[i].allocator = allocator;
		thread[i].max_allocation_size = max_allocation_size;
		thread[i].min_allocation_size = min_allocation_size;
		thread[i].iterations = iterations;
		thread[i].pool_size = pool_size;
		thread[i].random = random;
		thread[i].memory_index = i + 1;
	}

	pthread_t allocator_thread_id[8];

	for (int i = 0; i != thread_count; ++i) {
		if (pthread_create(&allocator_thread_id[i], NULL, callback, (void *)&thread[i]) != 0) {
			return 0;
		}
	}

	for (int i = 0; i != thread_count; ++i) {
		if (pthread_join(allocator_thread_id[i], NULL) != 0) {
			result = 0;
		}
	}

	tloc_index index = 0;
	while (thread_memory.memory[index]) {
		tloc_free_memory(thread_memory.memory[index]);
		index++;
	}

	return 1;
}
#endif

int main() {

	tloc_random random;
	tloc_size time = (tloc_size)clock() * 1000;
	_ReSeed(&random, time);
	//_ReSeed(&random, 257000);

	size_t size_of_header = sizeof(tloc_header);
	size_t size_of_size = sizeof(tloc_size);

#if defined(TLOC_THREAD_SAFE)
	PrintTestResult("Test: Multithreading test, 2 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, tloc__MEGABYTE(128), tloc__MINIMUM_BLOCK_SIZE, tloc__KILOBYTE(256), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, tloc__MEGABYTE(128), tloc__MINIMUM_BLOCK_SIZE, tloc__KILOBYTE(256), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, tloc__MEGABYTE(128), tloc__MINIMUM_BLOCK_SIZE, tloc__KILOBYTE(256), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(1), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(1), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(1), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(2), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(2), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(2), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(10), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(10), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, tloc__MEGABYTE(256), tloc__MINIMUM_BLOCK_SIZE, tloc__MEGABYTE(10), 8, &random));
#endif
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 16b - 256kb", TestManyAllocationsAndFreesAddPools(1000, tloc__MEGABYTE(128), tloc__MINIMUM_BLOCK_SIZE, tloc__KILOBYTE(256), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAllocationsAndFreesAddPools(1000, tloc__MEGABYTE(128), tloc__MEGABYTE(2), tloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks one by one resulting in 1 block left at the end after merges", TestAllocatingUntilOutOfSpaceThenFreeAll(1000, tloc__MEGABYTE(128), tloc__KILOBYTE(128), tloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks and remove the pool", TestRemovingPool(1000, tloc__MEGABYTE(128), tloc__KILOBYTE(128), tloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks and remove the pool", TestRemovingExtraPool(1000, tloc__MEGABYTE(128), tloc__KILOBYTE(128), tloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Multiple same size block allocations and frees", TestAllocateFreeSameSizeBlocks());
	PrintTestResult("Test: Pool passed to initialiser is too small", TestPoolTooSmall());
	PrintTestResult("Test: Non aligned memory passed to Initialiser", TestNonAlignedMemoryPool());
	PrintTestResult("Test: Attempt to allocate more memory than is available in one go", TestAllocateSingleOverAllocate());
	PrintTestResult("Test: Attempt to allocate more memory than is available with multiple attempts", TestAllocateMultiOverAllocate());
	PrintTestResult("Test: Attempt to allocate memory that is below minimum block size", TestAllocationTooSmall());
	PrintTestResult("Test: Attempt to reallocate memory", TestReAllocation());
	PrintTestResult("Test: Attempt to reallocate memory of null pointer (should just allocate instead)", TestReAllocationOfNullPtr());
	PrintTestResult("Test: Attempt to reallocate where it has to fall back to allocate and copy", TestReAllocationFallbackToAllocateAndCopy());
	PrintTestResult("Test: Multiple same size block allocations and frees", TestAllocateFreeSameSizeBlocks());
	//PrintTestResult("Test: Try to free an invalid allocation address", TestFreeingAnInvalidAllocation());
	//PrintTestResult("Test: Detect memory corruption by writing outside of bounds of an allocation (after)", TestMemoryCorruptionDetection());
	//PrintTestResult("Test: Detect memory corruption by writing outside of bounds of an allocation (before)", TestMemoryCorruptionDetection2());
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 16b - 256kb", TestManyAllocationsAndFreesAddPools(1000, tloc__MEGABYTE(128), tloc__MINIMUM_BLOCK_SIZE, tloc__KILOBYTE(256), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 64kb - 1MB", TestManyAllocationsAndFreesAddPools(1000, tloc__MEGABYTE(128), 64 * 1024, tloc__MEGABYTE(1), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 1MB - 2MB", TestManyAllocationsAndFreesAddPools(1000, tloc__MEGABYTE(128), tloc__MEGABYTE(1), tloc__MEGABYTE(2), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAllocationsAndFreesAddPools(1000, tloc__MEGABYTE(128), tloc__MEGABYTE(2), tloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Many random allocations and frees, go oom: 1000 iterations, 1GB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, tloc__GIGABYTE(1), tloc__KILOBYTE(256), tloc__MEGABYTE(50), &random));
	PrintTestResult("Test: Many random allocations and frees, go oom: 1000 iterations, 512MB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, tloc__MEGABYTE(512), tloc__KILOBYTE(256), tloc__MEGABYTE(25), &random));
	//PrintTestResult("Test: Allocations until full, then free and allocate randomly for 10000 iterations, 128MB pool size, max allocation: 128kb - 10MB", TestAllocatingUntilOutOfSpaceThenRandomFreesAndAllocations(1000, tloc__MEGABYTE(128), tloc__KILOBYTE(128), tloc__MEGABYTE(10), &random));
#if defined(tloc__64BIT)
	PrintTestResult("Test: Create a large (>4gb) memory pool, and allocate half of it", TestAllocation64bit());
#endif
	return 0;
}

#endif
