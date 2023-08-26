#include <stdio.h>
#include <time.h>

//#define PKT_DEV_MODE

#if !defined(PKT_DEV_MODE)
#define PKT_ERROR_COLOR "\033[90m"
#define PKT_IMPLEMENTATION
//#define PKT_OUTPUT_ERROR_MESSAGES
//#define PKT_THREAD_SAFE
#define PKT_ENABLE_REMOTE_MEMORY
#define PKT_MAX_SIZE_INDEX 35		//max block size 34GB
#include "pkt_allocator.h"
#define _TIMESPEC_DEFINED
#ifdef _WIN32
#ifdef PKT_THREAD_SAFE	
#include <pthread.h>
typedef void *(PTW32_CDECL *pkt__allocation_thread)(void*);
#endif
#include <windows.h>
#define pkt_sleep(seconds) Sleep(seconds)
#else
#ifdef PKT_THREAD_SAFE	
#include <pthread.h>
typedef void *( *pkt__allocation_thread)(void*);
#endif
#include <unistd.h>
#define pkt_sleep(seconds) sleep(seconds)
#endif

#define pkt_free_memory(memory) if(memory) free(memory);
#define TWO63 0x8000000000000000u 
#define TWO64f (TWO63*2.0)

//Debugging and validation
typedef void(*pkt__block_output)(void* ptr, size_t size, int used, void* user, int is_final_output);

typedef struct pkt_random {
	unsigned long long seeds[2];
} pkt_random;

void _Advance(pkt_random *random) {
	unsigned long long s1 = random->seeds[0];
	unsigned long long s0 = random->seeds[1];
	random->seeds[0] = s0;
	s1 ^= s1 << 23; // a
	random->seeds[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
}

void _ReSeed(pkt_random *random, unsigned long long seed) {
	random->seeds[0] = seed;
	random->seeds[1] = seed * 2;
	_Advance(random);
}

double _Generate(pkt_random *random) {
	unsigned long long s1 = random->seeds[0];
	unsigned long long s0 = random->seeds[1];
	unsigned long long result = s0 + s1;
	random->seeds[0] = s0;
	s1 ^= s1 << 23; // a
	random->seeds[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
	return (double)result / TWO64f;
}

unsigned long long _pkt_random_range(pkt_random *random, unsigned long long max) {
	double g = _Generate(random);
	double a = g * (double)max;
	return (unsigned long long)a;
};

void PrintTestResult(const char *message, int result) {
	printf("%s", message);
	printf("%s [%s]\033[0m\n", result == 0 ? "\033[31m" : "\033[32m", result == 0 ? "FAILED" : "PASSED");
}

static void pkt__output(void* ptr, size_t size, int free, void* user, int is_final_output)
{
	(void)user;
	pkt_header *block = (pkt_header*)ptr;
    printf("\t%p %s size: %zi (%p), (%p), (%p)\n", ptr, free ? "free" : "used", size, ptr, size ? block->next_free_block : 0, size ? block->prev_free_block : 0);
	if (is_final_output) {
		printf("\t------------- * ---------------\n");
	}
}

//Some helper functions for debugging
//Makes sure that all blocks in the segregated list of free blocks are all valid
pkt__error_codes pkt_VerifySegregatedLists(pkt_allocator *allocator) {
	for (int fli = 0; fli != pkt__FIRST_LEVEL_INDEX_COUNT; ++fli) {
		for (int sli = 0; sli != pkt__SECOND_LEVEL_INDEX_COUNT; ++sli) {
			pkt_header *block = allocator->segregated_lists[fli][sli];
			if (block->size) {
				pkt_index size_fli, size_sli;
				pkt__map(pkt__block_size(block), &size_fli, &size_sli);
				if (size_fli != fli && size_sli != sli) {
					return pkt__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST;
				}
			}
			if (block == pkt__null_block(allocator)) {
				continue;
			}
		}
	}
	return pkt__OK;
}

//Search the segregated list of free blocks for a given block
pkt_bool pkt_BlockExistsInSegregatedList(pkt_allocator *allocator, pkt_header* block) {
	for (int fli = 0; fli != pkt__FIRST_LEVEL_INDEX_COUNT; ++fli) {
		for (int sli = 0; sli != pkt__SECOND_LEVEL_INDEX_COUNT; ++sli) {
			pkt_header *current = allocator->segregated_lists[fli][sli];
			while (current != pkt__null_block(allocator)) {
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
pkt__error_codes pkt_VerifyBlocks(pkt_header *first_block, pkt__block_output output_function, void *user_data) {
	pkt_header *current_block = first_block;
	while (!pkt__is_last_block_in_pool(current_block)) {
		if (output_function) {
			pkt__output(current_block, pkt__block_size(current_block), pkt__is_free_block(current_block), user_data, 0);
		}
		pkt_header *last_block = current_block;
		current_block = pkt__next_physical_block(current_block);
		if (last_block != current_block->prev_physical_block) {
			return pkt__PHYSICAL_BLOCK_MISALIGNMENT;
		}
	}
	if (output_function) {
		pkt__output(current_block, pkt__block_size(current_block), pkt__is_free_block(current_block), user_data, 1);
	}
	return pkt__OK;
}

pkt__error_codes pkt_VerifyRemoteBlocks(pkt_header *first_block, pkt__block_output output_function, void *user_data) {
	pkt_header *current_block = first_block;
	int count = 0;
	while (!pkt__is_last_block_in_pool(current_block)) {
		void *remote_block = pkt_BlockUserExtensionPtr(current_block);
		if (output_function) {
			output_function(current_block, pkt__block_size(current_block), pkt__is_free_block(current_block), remote_block, ++count);
		}
		pkt_header *last_block = current_block;
		current_block = pkt__next_physical_block(current_block);
		if (last_block != current_block->prev_physical_block) {
			return pkt__PHYSICAL_BLOCK_MISALIGNMENT;
		}
	}
	return pkt__OK;
}

pkt_header *pkt_SearchList(pkt_allocator *allocator, pkt_header *search) {
	pkt_header *current_block = pkt__allocator_first_block(allocator);
	if (search == current_block) {
		return current_block;
	}
	while (!pkt__is_last_block_in_pool(current_block)) {
		if (search == current_block) {
			return current_block;
		}
		current_block = pkt__next_physical_block(current_block);
	}
	return current_block == search ? current_block : 0;
}

//Test if passing a memory pool that is too small to the initialiser is handled gracefully
int TestPoolTooSmall(void) {
	void *memory = malloc(1024);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, 1024);
	if (!allocator) {
		pkt_free_memory(memory);
		return 1;
	}
	pkt_free_memory(memory);
	return 0;
}

//Test if trying to free an invalid memory block fails gracefully
int TestFreeingAnInvalidAllocation(void) {
	int result = 0;
	void *memory = malloc(pkt__MEGABYTE(1));
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pkt__MEGABYTE(1));
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (allocator) {
		void *allocation = malloc(pkt__KILOBYTE(1));
		if (!pkt_Free(allocator, allocation)) {
			result = 1;
		}
		pkt_free_memory(allocation);
	}
	pkt_free_memory(memory);
	return result;
}

//Write outside the bounds of an allocation
int TestMemoryCorruptionDetection(void) {
	int result = 0;
	void *memory = malloc(pkt__MEGABYTE(1));
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pkt__MEGABYTE(1));
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (allocator) {
		int *allocation = pkt_Allocate(allocator, sizeof(int) * 10);
		for (int i = 0; i != 20; ++i) {
			allocation[i] = rand();
		}
		if (!pkt_Free(allocator, allocation)) {
			result = 1;
		}
	}
	pkt_free_memory(memory);
	return result;
}

//Write outside the bounds of an allocation
int TestMemoryCorruptionDetection2(void) {
	int result = 0;
	void *memory = malloc(pkt__MEGABYTE(1));
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pkt__MEGABYTE(1));
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (allocator) {
		int *allocation = pkt_Allocate(allocator, sizeof(int) * 10);
		for (int i = -5; i != 10; ++i) {
			allocation[i] = rand();
		}
		if (!pkt_Free(allocator, allocation)) {
			result = 1;
		}
	}
	pkt_free_memory(memory);
	return result;
}

int TestNonAlignedMemoryPool(void) {
	void *memory = malloc(1023);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, 1023);
	if (!allocator) {
		pkt_free_memory(memory);
		return 1;
	}
	pkt_free_memory(memory);
	return 0;
}

int TestAllocateSingleOverAllocate(void) {
	pkt_size size = pkt__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = pkt_Allocate(allocator, size);
		if (allocation) {
			result = 0;
		}
	}
	pkt_free_memory(memory);
	return result;
}

int TestAllocateMultiOverAllocate(void) {
	pkt_size size = pkt__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
	}
	pkt_size allocated = 0;
	while (pkt_Allocate(allocator, 1024)) {
		allocated += 1024;
		if (pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) != pkt__OK) {
			result = 0;
			break;
		}
		if (allocated > size) {
			result = 0;
			break;
		}
	}
	pkt_free_memory(memory);
	return result;
}

//This tests that free blocks in the segregated list will be exhausted first before using the main pool for allocations
int TestAllocateFreeSameSizeBlocks(void) {
	pkt_size size = pkt__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
	}
	void *allocations[40];
	for (int i = 0; i != 20; ++i) {
		allocations[i] = pkt_Allocate(allocator, 1024);
		if (!allocations[i]) {
			result = 0;
		}
	}
	//Free every second one so that blocks don't get merged
	for (int i = 0; i != 20; i += 2) {
		pkt_Free(allocator, allocations[i]);
		pkt__error_codes error = pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0);
		assert(error == pkt__OK);
		allocations[i] = 0;
	}
	for (int i = 0; i != 40; ++i) {
		allocations[i] = pkt_Allocate(allocator, 1024);
		pkt__error_codes error = pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0);
		assert(error == pkt__OK);
		if (!allocations[i]) {
			result = 0;
		}
	}
	for (int i = 0; i != 40; ++i) {
		if (allocations[i]) {
			pkt__error_codes error = pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0);
			assert(error == pkt__OK);
			pkt_Free(allocator, allocations[i]);
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
	}
	pkt_free_memory(memory);
	return result;
}

//Test allocating some memory that is too small
int TestAllocationTooSmall(void) {
	pkt_size size = pkt__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = pkt_Allocate(allocator, 4);
		if (pkt__block_size(pkt__block_from_allocation(allocation)) == pkt__MINIMUM_BLOCK_SIZE) {
			result = 1;
		}
		else {
			result = 0;
		}
	}
	pkt_free_memory(memory);
	return result;
}

int TestReAllocation(void) {
	pkt_size size = pkt__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = pkt_Allocate(allocator, 1024);
		if (!allocation) {
			result = 0;
		}
		else {
			allocation = pkt_Reallocate(allocator, allocation, 2048);
			if (!allocation) {
				result = 0;
			}
			else if (pkt__block_size(pkt__block_from_allocation(allocation)) != 2048) {
				result = 0;
			}
		}
	}
	pkt_free_memory(memory);
	return result;
}

int TestReAllocationFallbackToAllocateAndCopy(void) {
	pkt_size size = pkt__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation1 = pkt_Allocate(allocator, 1024);
		void *allocation2 = pkt_Allocate(allocator, 1024);
		allocation1 = pkt_Reallocate(allocator, allocation1, 2048);
		if (!allocation1) {
			result = 0;
		}
	}
	pkt_free_memory(memory);
	return result;
}

int TestReAllocationOfNullPtr(void) {
	pkt_size size = pkt__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = 0;
		allocation = pkt_Reallocate(allocator, allocation, 1024);
		if (!allocation) {
			result = 0;
		}
		else {
			result = 1;
		}
	}
	pkt_free_memory(memory);
	return result;
}

int TestManyAllocationsAndFreesDummy(pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size) {
	int result = 1;
	pkt_size allocations[100];
	memset(allocations, 0, sizeof(pkt_size) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		if (allocations[index]) {
			allocations[index] = 0;
		}
		else {
			pkt_size allocation_size = (rand() % max_allocation_size) + pkt__MINIMUM_BLOCK_SIZE;
			allocations[index] = allocation_size;
		}
	}
	return result;
}

int TestFreeAllBuffersAndPools(pkt_allocator *allocator, void *memory[8], void *buffers[100]) {
	int result = 1;

	for (int i = 0; i != 100; ++i) {
		if (buffers[i]) {
			if (!pkt_Free(allocator, buffers[i])) {
				result = 0;
				break;
			}
			else {
				buffers[i] = 0;
			}
		}
	}

	for (int i = 0; i != 8; ++i) {
		if (memory[i]) {
			if (i == 0) {
				pkt_VerifyBlocks(pkt__allocator_first_block(allocator), pkt__output, 0);
				result = pkt_RemovePool(allocator, pkt_GetPool(allocator));
			}
			else {
				pkt_VerifyBlocks(pkt__first_block_in_pool(memory[i]), pkt__output, 0);
				result = pkt_RemovePool(allocator, memory[i]);
			}
		}
	}

	for (int i = 0; i != 8; ++i) {
		if (memory[i]) {
			pkt_free_memory(memory[i]);
		}
	}

	return result;
}

//Do lots of allocations and frees of random sizes
int TestManyAllocationsAndFrees(pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pool_size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
		pkt_free_memory(memory);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				pkt_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//pkt_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				allocations[index] = pkt_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		pkt_free_memory(memory);
	}
	return result;
}

int TestManyAllocationsAndFreesAddPools(pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	void *memory[8];
	memset(memory, 0, sizeof(void*) * 8);
	int memory_index = 0;
	memory[memory_index] = malloc(pool_size);
	memset(memory[memory_index], 0, pool_size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory[memory_index++], pool_size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
		pkt_free_memory(memory[0]);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				pkt_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//pkt_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				allocations[index] = pkt_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
				else if(memory_index < 8) {
					//We ran out of memory, add a new pool
					memory[memory_index] = malloc(pool_size);
                    if(memory[memory_index]) {
                        pkt_AddPool(allocator, memory[memory_index++], pool_size);
                        allocations[index] = pkt_Allocate(allocator, allocation_size);
                        if (!allocations[index]) {
                            result = 0;
                            break;
                        }
                    }
				}
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		result = TestFreeAllBuffersAndPools(allocator, memory, allocations);
	}
	return result;
}

int TestAllocatingUntilOutOfSpaceThenRandomFreesAndAllocations(pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pool_size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
		pkt_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = pkt_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 1000;
			if (allocations[index]) {
				pkt_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				allocations[index] = pkt_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		pkt_free_memory(memory);
	}
	return result;
}

//Test that after allocating lots of allocations and freeing them all, there should only be one free allocation
//because all free blocks should get merged
int TestAllocatingUntilOutOfSpaceThenFreeAll(pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pool_size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
		pkt_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = pkt_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				pkt_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		pkt_header *first_block = pkt__allocator_first_block(allocator);
		result = pkt__is_last_block_in_pool(pkt__next_physical_block(first_block));
		pkt_free_memory(memory);
	}
	return result;
}

int TestRemovingPool(pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pool_size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
		pkt_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = pkt_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				pkt_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		if (!pkt_RemovePool(allocator, pkt_GetPool(allocator))) {
			result = 0;
		}
		pkt_free_memory(memory);
	}
	return result;
}

int TestRemovingExtraPool(pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	pkt_pool *extra_pool = 0;
	memset(memory, 0, pool_size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, pool_size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
		pkt_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = pkt_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				void *extra_memory = malloc(pool_size);
				extra_pool = pkt_AddPool(allocator, extra_memory, pool_size);
				allocations[i] = pkt_Allocate(allocator, allocation_size);
				break;
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				pkt_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(pkt_VerifyBlocks(pkt__allocator_first_block(allocator), 0, 0) == pkt__OK);
		}
		if (!pkt_RemovePool(allocator, extra_pool)) {
			result = 0;
		}
		if (!pkt_RemovePool(allocator, pkt_GetPool(allocator))) {
			result = 0;
		}
		pkt_free_memory(memory);
		if (extra_pool) {
			pkt_free_memory(extra_pool);
		}
	}
	return result;
}

//64bit tests
#if defined(pkt__64BIT)
//Allocate a large block
int TestAllocation64bit(void) {
	pkt_size size = (1024ull * 1024ull * 1024ull * 6ull);	//6 gb
	int result = 1;
	void* memory = malloc(size);
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(memory, size);
	assert(pkt_VerifySegregatedLists(allocator) == pkt__OK);
	if (!allocator) {
		result = 0;
	}
	if(allocator) {
		if (pkt_Allocate(allocator, size / 2)) {
			result = 1;
		}
	}
	pkt_free_memory(memory);
	return result;
}
#endif

typedef struct pkt_thread_test {
	pkt_allocator *allocator;
	void *allocations[100];
	pkt_random *random;
	pkt_uint iterations;
	pkt_size pool_size;
	pkt_size min_allocation_size;
	pkt_size max_allocation_size;
	pkt_index memory_index;
} pkt_thread_test;

#ifdef PKT_THREAD_SAFE
struct pkt_memory_threads {
	void *memory[9];
	volatile pkt_thread_access access;
};

struct pkt_memory_threads thread_memory;

// Function that will be executed by the thread
void *AllocationWorker(void *arg) {
	pkt_thread_test *thread_test = (pkt_thread_test*)arg;
	for (int i = 0; i != thread_test->iterations; ++i) {
		int index = rand() % 100;
		if (thread_test->allocations[index]) {
			pkt_Free(thread_test->allocator, thread_test->allocations[index]);
			thread_test->allocations[index] = 0;
		}
		else {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(thread_test->random, thread_test->max_allocation_size - thread_test->min_allocation_size) + thread_test->min_allocation_size;
			thread_test->allocations[index] = pkt_Allocate(thread_test->allocator, allocation_size);
			if (thread_test->allocations[index]) {
				//Do a memset set to test if we're overwriting block headers
				memset(thread_test->allocations[index], 7, allocation_size);
			}
		}
	}
	return 0;
}

void *AllocationWorkerAddPool(void *arg) {
	pkt_thread_test *thread_test = (pkt_thread_test*)arg;
	for (int i = 0; i != thread_test->iterations; ++i) {
		int index = rand() % 100;
		if (thread_test->allocations[index]) {
			pkt_Free(thread_test->allocator, thread_test->allocations[index]);
			thread_test->allocations[index] = 0;
		}
		else {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(thread_test->random, thread_test->max_allocation_size - thread_test->min_allocation_size) + thread_test->min_allocation_size;
			thread_test->allocations[index] = pkt_Allocate(thread_test->allocator, allocation_size);
			if (thread_test->allocations[index]) {
				//Do a memset set to test if we're overwriting block headers
				memset(thread_test->allocations[index], 7, allocation_size);
			}
			else if (thread_memory.memory[thread_test->memory_index] == 0) {
				//We ran out of memory, add a new pool
				pkt_thread_access original_access = thread_memory.access;
				pkt_thread_access access = pkt__compare_and_exchange(&thread_memory.access, 1, original_access);
				if (original_access == access) {
					pkt_index index = thread_test->memory_index;
					thread_memory.memory[index] = malloc(thread_test->pool_size);
					pkt_AddPool(thread_test->allocator, thread_memory.memory[index], thread_test->pool_size);
					printf("\033[34mThread %i added pool\033[0m\n", index);
					thread_test->allocations[index] = pkt_Allocate(thread_test->allocator, allocation_size);
					thread_memory.access = 0;
				}
				else {
					do {} while (thread_memory.access == 1);
					thread_test->allocations[index] = pkt_Allocate(thread_test->allocator, allocation_size);
				}
			}
		}
	}
	return 0;
}

int TestMultithreading(pkt__allocation_thread callback, pkt_uint iterations, pkt_size pool_size, pkt_size min_allocation_size, pkt_size max_allocation_size, int thread_count, pkt_random *random) {
	int result = 1;

	thread_memory.memory[0] = malloc(pool_size);
	pkt_thread_test thread[8];
	pkt_allocator *allocator = pkt_InitialiseAllocatorWithPool(thread_memory.memory[0], pool_size);
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

	pkt_index index = 0;
	while (thread_memory.memory[index]) {
		pkt_free_memory(thread_memory.memory[index]);
		index++;
	}

	return 1;
}
#endif

#ifdef PKT_ENABLE_REMOTE_MEMORY
//Test remote pools
typedef struct remote_memory_pools {
	void *memory_pools[8];
	void *range_pools[8];
	pkt_size pool_sizes[8];
	pkt_uint pool_count;
} remote_memory_pools;

typedef struct remote_buffer {
	pkt_size size;
	pkt_size offset_from_pool;
	void *pool;
	void *data;
} remote_buffer;

void on_add_pool(void *user_data, void *block) {
	remote_memory_pools *pools = (remote_memory_pools*)user_data;
	remote_buffer *buffer = (remote_buffer*)block;
	buffer->pool = pools->memory_pools[pools->pool_count];
	buffer->size = pools->pool_sizes[pools->pool_count++];
	buffer->offset_from_pool = 0;
}

void on_split_block(void *user_data, pkt_header* block, pkt_header *trimmed_block, pkt_size remote_size) {
	remote_memory_pools *pools = (remote_memory_pools*)user_data;
	remote_buffer *buffer = pkt_BlockUserExtensionPtr(block);
	remote_buffer *trimmed_buffer = pkt_BlockUserExtensionPtr(trimmed_block);
	trimmed_buffer->size = buffer->size - remote_size;
	buffer->size = remote_size;
	trimmed_buffer->pool = buffer->pool;
	trimmed_buffer->offset_from_pool = buffer->offset_from_pool + buffer->size;
	buffer->data = (void*)((char*)buffer->pool + buffer->offset_from_pool);
}

void on_reallocation_copy(void *user_data, pkt_header* block, pkt_header *new_block) {
	remote_memory_pools *pools = (remote_memory_pools*)user_data;
	remote_buffer *buffer = pkt_BlockUserExtensionPtr(block);
	remote_buffer *new_buffer = pkt_BlockUserExtensionPtr(new_block);
	new_buffer->data = (void*)((char*)new_buffer->pool + new_buffer->offset_from_pool);
	memcpy(new_buffer->data, buffer->data, buffer->size);
}

static void pkt__output_buffer_info(void* ptr, size_t size, int free, void* user, int count)
{
	remote_buffer *buffer = (remote_buffer*)user;
	pkt_header *block = (pkt_header*)ptr;
	printf("%i) \t%s size: \t%zi \tbuffer size: %zu \toffset: %zu \n", count, free ? "free" : "used", size, buffer->size, buffer->offset_from_pool);
}

int TestFreeAllRemoteBuffersAndPools(pkt_allocator *allocator, remote_memory_pools *pools, remote_buffer *buffers[100]) {
	int result = 1;
	for (int i = 0; i != 100; ++i) {
		if (buffers[i]) {
			if (!pkt_FreeRemote(allocator, buffers[i])) {
				result = 0;
				break;
			}
			else {
				buffers[i] = 0;
			}
		}
	}
	for (int i = 0; i != pools->pool_count; ++i) {
		pkt_VerifyRemoteBlocks(pkt__first_block_in_pool(pools->range_pools[i]), pkt__output_buffer_info, 0);
		result = pkt_RemovePool(allocator, pools->range_pools[i]);
		pkt_free_memory(pools->range_pools[i]);
		pkt_free_memory(pools->memory_pools[i]);
	}

	return result;
}

int TestRemoteMemoryBlockManagement(pkt_uint iterations, pkt_size pool_size, pkt_size minimum_remote_allocation_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	pkt_allocator *allocator;
	void *allocator_memory = malloc(pkt_AllocatorSize());
	allocator = pkt_InitialiseAllocator(allocator_memory);
	pkt_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	pkt_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	pkt_size memory_sizes[4] = { pkt__MEGABYTE(1), pkt__MEGABYTE(2), pkt__MEGABYTE(3), pkt__MEGABYTE(4) };
	pkt_size range_pool_size = pkt_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	pkt_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		if (buffers[index]) {
			if (!pkt_FreeRemote(allocator, buffers[index])) {
				result = 0;
				break;
			}
			buffers[index] = 0;
		}
		else {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			//pkt_size allocation_size = memory_sizes[rand() % 4];
			buffers[index] = pkt_AllocateRemote(allocator, allocation_size);
			if (!buffers[index]) {
				//Ran out of room in the pool
				if (pools.pool_count == 8) {
					continue;
				}
				pools.pool_sizes[pools.pool_count] = pool_size;
				range_pool_size = pkt_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
				pools.range_pools[pools.pool_count] = malloc(range_pool_size);
				pools.memory_pools[pools.pool_count] = malloc(pool_size);
				pkt_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
				buffers[index] = pkt_AllocateRemote(allocator, allocation_size);
			}
			else {
				buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
			}
		}
		for (int c = 0; c != pools.pool_count; ++c) {
			assert(pkt_VerifyRemoteBlocks(pkt__first_block_in_pool(pools.range_pools[c]), 0, 0) == pkt__OK);
		}
	}
	//remote_buffer *test = pkt_AllocateRemote(allocator, pkt__MEGABYTE(32));
	TestFreeAllRemoteBuffersAndPools(allocator, &pools, buffers);
	pkt_free_memory(allocator_memory);
	return result;
}

int TestRemoteMemoryReallocation(pkt_uint iterations, pkt_size pool_size, pkt_size minimum_remote_allocation_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	pkt_allocator *allocator;
	void *allocator_memory = malloc(pkt_AllocatorSize());
	allocator = pkt_InitialiseAllocator(allocator_memory);
	pkt_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	pkt_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	pkt_size memory_sizes[4] = { pkt__MEGABYTE(1), pkt__MEGABYTE(2), pkt__MEGABYTE(3), pkt__MEGABYTE(4) };
	pkt_size range_pool_size = pkt_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	pkt_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	buffers[0] = pkt_ReallocateRemote(allocator, buffers[0], pkt__KILOBYTE(1));
	buffers[0]->data = (void*)((char*)pools.memory_pools[0] + buffers[0]->offset_from_pool);
	buffers[1] = pkt_ReallocateRemote(allocator, buffers[1], pkt__KILOBYTE(1));
	buffers[1]->data = (void*)((char*)pools.memory_pools[0] + buffers[1]->offset_from_pool);
	buffers[0] = pkt_ReallocateRemote(allocator, buffers[0], pkt__KILOBYTE(2));
	buffers[0]->data = (void*)((char*)pools.memory_pools[0] + buffers[0]->offset_from_pool);
	buffers[1] = pkt_ReallocateRemote(allocator, buffers[1], pkt__KILOBYTE(2));
	buffers[1]->data = (void*)((char*)pools.memory_pools[0] + buffers[1]->offset_from_pool);
	//remote_buffer *test = pkt_AllocateRemote(allocator, pkt__MEGABYTE(32));
	for (int c = 0; c != pools.pool_count; ++c) {
		pkt_VerifyRemoteBlocks(pkt__first_block_in_pool(pools.range_pools[c]), pkt__output_buffer_info, 0);
		pkt_free_memory(pools.range_pools[c]);
		pkt_free_memory(pools.memory_pools[c]);
	}
	pkt_free_memory(allocator_memory);
	return result;
}

int TestRemoteMemoryReallocationIterations(pkt_uint iterations, pkt_size pool_size, pkt_size minimum_remote_allocation_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	pkt_allocator *allocator;
	void *allocator_memory = malloc(pkt_AllocatorSize());
	allocator = pkt_InitialiseAllocatorForRemote(allocator_memory);
	pkt_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	pkt_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	pkt_size range_pool_size = pkt_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	pkt_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
		buffers[index] = pkt_ReallocateRemote(allocator, buffers[index], allocation_size);
		if (!buffers[index]) {
			//Ran out of room in the pool
			if (pools.pool_count == 8) {
				continue;
			}
			pools.pool_sizes[pools.pool_count] = pool_size;
			range_pool_size = pkt_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
			pools.range_pools[pools.pool_count] = malloc(range_pool_size);
			pools.memory_pools[pools.pool_count] = malloc(pool_size);
			pkt_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
			buffers[index] = pkt_ReallocateRemote(allocator, buffers[index], allocation_size);
			if (buffers[index]) {
				buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
			}
		}
		else {
			buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
		}
		for (int c = 0; c != pools.pool_count; ++c) {
			assert(pkt_VerifyRemoteBlocks(pkt__first_block_in_pool(pools.range_pools[c]), 0, 0) == pkt__OK);
		}
	}
	//remote_buffer *test = pkt_AllocateRemote(allocator, pkt__MEGABYTE(32));
	for (int c = 0; c != pools.pool_count; ++c) {
		//pkt_VerifyRemoteBlocks(pkt__first_block_in_pool(pools.range_pools[c]), pkt__output_buffer_info, 0);
		pkt_free_memory(pools.range_pools[c]);
		pkt_free_memory(pools.memory_pools[c]);
	}
	pkt_free_memory(allocator_memory);
	return result;
}

int TestRemoteMemoryReallocationIterationsFreeing(pkt_uint iterations, pkt_size pool_size, pkt_size minimum_remote_allocation_size, pkt_size min_allocation_size, pkt_size max_allocation_size, pkt_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	pkt_allocator *allocator;
	void *allocator_memory = malloc(pkt_AllocatorSize());
	allocator = pkt_InitialiseAllocator(allocator_memory);
	pkt_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	pkt_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	pkt_size range_pool_size = pkt_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	pkt_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		if (buffers[index] && buffers[index]->size > min_allocation_size + ((max_allocation_size - min_allocation_size) / 2)) {
			if (!pkt_FreeRemote(allocator, buffers[index])) {
				result = 0;
				break;
			}
			buffers[index] = 0;
		}
		else if(!buffers[index]) {
			pkt_size allocation_size = (pkt_size)_pkt_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			//pkt_size allocation_size = memory_sizes[rand() % 4];
			remote_buffer *new_allocation = pkt_ReallocateRemote(allocator, buffers[index], allocation_size);
			if (!new_allocation) {
				//Ran out of room in the pool
				if (pools.pool_count == 8) {
					continue;
				}
				pools.pool_sizes[pools.pool_count] = pool_size;
				range_pool_size = pkt_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
				pools.range_pools[pools.pool_count] = malloc(range_pool_size);
				pools.memory_pools[pools.pool_count] = malloc(pool_size);
				pkt_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
				new_allocation = pkt_ReallocateRemote(allocator, buffers[index], allocation_size);
				if (new_allocation) {
					buffers[index] = new_allocation;
					buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
				}
			}
			else {
				buffers[index] = new_allocation;
				buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
			}
		}
		for (int c = 0; c != pools.pool_count; ++c) {
			assert(pkt_VerifyRemoteBlocks(pkt__first_block_in_pool(pools.range_pools[c]), 0, 0) == pkt__OK);
		}
	}
	result = TestFreeAllRemoteBuffersAndPools(allocator, &pools, buffers);
	pkt_free_memory(allocator_memory);
	return result;
}
#endif

int main() {

	pkt_random random;
	pkt_size time = (pkt_size)clock() * 1000;
	_ReSeed(&random, time);
	//_ReSeed(&random, 178000);
	//_ReSeed(&random, 123456);

#if defined(PKT_THREAD_SAFE)
	PrintTestResult("Test: Multithreading test, 2 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, pkt__MEGABYTE(128), pkt__MINIMUM_BLOCK_SIZE, pkt__KILOBYTE(256), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, pkt__MEGABYTE(128), pkt__MINIMUM_BLOCK_SIZE, pkt__KILOBYTE(256), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, pkt__MEGABYTE(128), pkt__MINIMUM_BLOCK_SIZE, pkt__KILOBYTE(256), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(1), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(1), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(1), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(2), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(2), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(2), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(10), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(10), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, pkt__MEGABYTE(256), pkt__MINIMUM_BLOCK_SIZE, pkt__MEGABYTE(10), 8, &random));
#endif
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 16b - 256kb", TestManyAllocationsAndFreesAddPools(1000, pkt__MEGABYTE(128), pkt__MINIMUM_BLOCK_SIZE, pkt__KILOBYTE(256), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAllocationsAndFreesAddPools(1000, pkt__MEGABYTE(128), pkt__MEGABYTE(2), pkt__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks one by one resulting in 1 block left at the end after merges", TestAllocatingUntilOutOfSpaceThenFreeAll(1000, pkt__MEGABYTE(128), pkt__KILOBYTE(128), pkt__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks and remove the pool", TestRemovingPool(1000, pkt__MEGABYTE(128), pkt__KILOBYTE(128), pkt__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks and remove the pool", TestRemovingExtraPool(1000, pkt__MEGABYTE(128), pkt__KILOBYTE(128), pkt__MEGABYTE(10), &random));
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
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 16b - 256kb", TestManyAllocationsAndFreesAddPools(1000, pkt__MEGABYTE(128), pkt__MINIMUM_BLOCK_SIZE, pkt__KILOBYTE(256), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 64kb - 1MB", TestManyAllocationsAndFreesAddPools(1000, pkt__MEGABYTE(128), 64 * 1024, pkt__MEGABYTE(1), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 1MB - 2MB", TestManyAllocationsAndFreesAddPools(1000, pkt__MEGABYTE(128), pkt__MEGABYTE(1), pkt__MEGABYTE(2), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAllocationsAndFreesAddPools(1000, pkt__MEGABYTE(128), pkt__MEGABYTE(2), pkt__MEGABYTE(10), &random));
	PrintTestResult("Test: Many random allocations and frees, go oom: 1000 iterations, 1GB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, pkt__GIGABYTE(1), pkt__KILOBYTE(256), pkt__MEGABYTE(50), &random));
	PrintTestResult("Test: Many random allocations and frees, go oom: 1000 iterations, 512MB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, pkt__MEGABYTE(512), pkt__KILOBYTE(256), pkt__MEGABYTE(25), &random));
	//PrintTestResult("Test: Allocations until full, then free and allocate randomly for 10000 iterations, 128MB pool size, max allocation: 128kb - 10MB", TestAllocatingUntilOutOfSpaceThenRandomFreesAndAllocations(1000, pkt__MEGABYTE(128), pkt__KILOBYTE(128), pkt__MEGABYTE(10), &random));
#if defined(pkt__64BIT)
	PrintTestResult("Test: Create a large (>4gb) memory pool, and allocate half of it", TestAllocation64bit());
#endif

#ifdef PKT_ENABLE_REMOTE_MEMORY
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 16b - 1k, add 1mb pools as needed.", TestRemoteMemoryBlockManagement(10000, pkt__MEGABYTE(1), 512, 16, pkt__KILOBYTE(1), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 8kb - 64kb, add 16mb pools as needed.", TestRemoteMemoryBlockManagement(10000, pkt__MEGABYTE(64), pkt__KILOBYTE(8), pkt__KILOBYTE(8), pkt__KILOBYTE(64), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 256kb - 2mb, add 64mb pools as needed.", TestRemoteMemoryBlockManagement(10000, pkt__MEGABYTE(64), pkt__KILOBYTE(256), pkt__KILOBYTE(256), pkt__MEGABYTE(2), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 1MB - 64mb, add 128mb pools as needed.", TestRemoteMemoryBlockManagement(10000, pkt__MEGABYTE(128), pkt__MEGABYTE(1), pkt__MEGABYTE(1), pkt__MEGABYTE(64), &random));
	PrintTestResult("Test: Remote memory management, Reallocation", TestRemoteMemoryReallocation(10000, pkt__MEGABYTE(16), 512, 16, pkt__KILOBYTE(1), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 512b - 4kb", TestRemoteMemoryReallocationIterations(10000, pkt__MEGABYTE(16), 512, 512, pkt__KILOBYTE(4), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 256kb - 2MB", TestRemoteMemoryReallocationIterations(10000, pkt__MEGABYTE(16), pkt__KILOBYTE(256), pkt__KILOBYTE(256), pkt__MEGABYTE(2), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 256kb - 4MB", TestRemoteMemoryReallocationIterations(10000, pkt__MEGABYTE(64), pkt__KILOBYTE(256), pkt__KILOBYTE(256), pkt__MEGABYTE(4), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 256kb - 4MB with Freeing", TestRemoteMemoryReallocationIterationsFreeing(10000, pkt__MEGABYTE(64), pkt__KILOBYTE(256), pkt__KILOBYTE(256), pkt__MEGABYTE(4), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 1MB - 64mb, add 128mb pools as needed.", TestRemoteMemoryReallocationIterationsFreeing(10000, pkt__MEGABYTE(128), pkt__MEGABYTE(1), pkt__MEGABYTE(1), pkt__MEGABYTE(16), &random));
#endif
	return 0;
}

#endif
