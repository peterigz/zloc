#include <stdio.h>
#include <time.h>

//#define ZLOC_DEV_MODE

#if !defined(ZLOC_DEV_MODE)
#define ZLOC_ERROR_COLOR "\033[90m"
#define ZLOC_IMPLEMENTATION
//#define ZLOC_OUTPUT_ERROR_MESSAGES
#define ZLOC_THREAD_SAFE
#define ZLOC_ENABLE_REMOTE_MEMORY
#define ZLOC_MAX_SIZE_INDEX 35		//max block size 34GB
#include "zloc.h"
#define _TIMESPEC_DEFINED
#ifdef _WIN32
#ifdef ZLOC_THREAD_SAFE	
#include <pthread.h>
typedef void *(PTW32_CDECL *zloc__allocation_thread)(void*);
#endif
#include <windows.h>
#define zloc_sleep(seconds) Sleep(seconds)
#else
#ifdef ZLOC_THREAD_SAFE	
#include <pthread.h>
typedef void *( *zloc__allocation_thread)(void*);
#endif
#include <unistd.h>
#define zloc_sleep(seconds) sleep(seconds)
#endif

#define zloc_free_memory(memory) if(memory) free(memory);
#define TWO63 0x8000000000000000u 
#define TWO64f (TWO63*2.0)

//Debugging and validation
typedef void(*zloc__block_output)(void* ptr, size_t size, int used, void* user, int is_final_output);

typedef struct zloc_random {
	unsigned long long seeds[2];
} zloc_random;

void _Advance(zloc_random *random) {
	unsigned long long s1 = random->seeds[0];
	unsigned long long s0 = random->seeds[1];
	random->seeds[0] = s0;
	s1 ^= s1 << 23; // a
	random->seeds[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
}

void _ReSeed(zloc_random *random, unsigned long long seed) {
	random->seeds[0] = seed;
	random->seeds[1] = seed * 2;
	_Advance(random);
}

double _Generate(zloc_random *random) {
	unsigned long long s1 = random->seeds[0];
	unsigned long long s0 = random->seeds[1];
	unsigned long long result = s0 + s1;
	random->seeds[0] = s0;
	s1 ^= s1 << 23; // a
	random->seeds[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
	return (double)result / TWO64f;
}

unsigned long long _zloc_random_range(zloc_random *random, unsigned long long max) {
	double g = _Generate(random);
	double a = g * (double)max;
	return (unsigned long long)a;
};

void PrintTestResult(const char *message, int result) {
	printf("%s", message);
	printf("%s [%s]\033[0m\n", result == 0 ? "\033[31m" : "\033[32m", result == 0 ? "FAILED" : "PASSED");
}

static void zloc__output(void* ptr, size_t size, int free, void* user, int is_final_output)
{
	(void)user;
	zloc_header *block = (zloc_header*)ptr;
    printf("\t%p %s size: %zi (%p), (%p), (%p)\n", ptr, free ? "free" : "used", size, ptr, size ? block->next_free_block : 0, size ? block->prev_free_block : 0);
	if (is_final_output) {
		printf("\t------------- * ---------------\n");
	}
}

//Some helper functions for debugging
//Makes sure that all blocks in the segregated list of free blocks are all valid
zloc__error_codes zloc_VerifySegregatedLists(zloc_allocator *allocator) {
	for (int fli = 0; fli != zloc__FIRST_LEVEL_INDEX_COUNT; ++fli) {
		for (int sli = 0; sli != zloc__SECOND_LEVEL_INDEX_COUNT; ++sli) {
			zloc_header *block = allocator->segregated_lists[fli][sli];
			if (block->size) {
				zloc_index size_fli, size_sli;
				zloc__map(zloc__block_size(block), &size_fli, &size_sli);
				if (size_fli != fli && size_sli != sli) {
					return zloc__WRONG_BLOCK_SIZE_FOUND_IN_SEGRATED_LIST;
				}
			}
			if (block == zloc__null_block(allocator)) {
				continue;
			}
		}
	}
	return zloc__OK;
}

//Search the segregated list of free blocks for a given block
zloc_bool zloc_BlockExistsInSegregatedList(zloc_allocator *allocator, zloc_header* block) {
	for (int fli = 0; fli != zloc__FIRST_LEVEL_INDEX_COUNT; ++fli) {
		for (int sli = 0; sli != zloc__SECOND_LEVEL_INDEX_COUNT; ++sli) {
			zloc_header *current = allocator->segregated_lists[fli][sli];
			while (current != zloc__null_block(allocator)) {
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
zloc__error_codes zloc_VerifyBlocks(zloc_header *first_block, zloc__block_output output_function, void *user_data) {
	zloc_header *current_block = first_block;
	while (!zloc__is_last_block_in_pool(current_block)) {
		if (output_function) {
			zloc__output(current_block, zloc__block_size(current_block), zloc__is_free_block(current_block), user_data, 0);
		}
		zloc_header *last_block = current_block;
		current_block = zloc__next_physical_block(current_block);
		if (last_block != current_block->prev_physical_block) {
			return zloc__PHYSICAL_BLOCK_MISALIGNMENT;
		}
	}
	if (output_function) {
		zloc__output(current_block, zloc__block_size(current_block), zloc__is_free_block(current_block), user_data, 1);
	}
	return zloc__OK;
}

zloc__error_codes zloc_VerifyRemoteBlocks(zloc_header *first_block, zloc__block_output output_function, void *user_data) {
	zloc_header *current_block = first_block;
	int count = 0;
	while (!zloc__is_last_block_in_pool(current_block)) {
		void *remote_block = zloc_BlockUserExtensionPtr(current_block);
		if (output_function) {
			output_function(current_block, zloc__block_size(current_block), zloc__is_free_block(current_block), remote_block, ++count);
		}
		zloc_header *last_block = current_block;
		current_block = zloc__next_physical_block(current_block);
		if (last_block != current_block->prev_physical_block) {
			return zloc__PHYSICAL_BLOCK_MISALIGNMENT;
		}
	}
	return zloc__OK;
}

zloc_header *zloc_SearchList(zloc_allocator *allocator, zloc_header *search) {
	zloc_header *current_block = zloc__allocator_first_block(allocator);
	if (search == current_block) {
		return current_block;
	}
	while (!zloc__is_last_block_in_pool(current_block)) {
		if (search == current_block) {
			return current_block;
		}
		current_block = zloc__next_physical_block(current_block);
	}
	return current_block == search ? current_block : 0;
}

//Test if passing a memory pool that is too small to the initialiser is handled gracefully
int TestPoolTooSmall(void) {
	void *memory = malloc(1024);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, 1024);
	if (!allocator) {
		zloc_free_memory(memory);
		return 1;
	}
	zloc_free_memory(memory);
	return 0;
}

//Test if trying to free an invalid memory block fails gracefully
int TestFreeingAnInvalidAllocation(void) {
	int result = 0;
	void *memory = malloc(zloc__MEGABYTE(1));
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, zloc__MEGABYTE(1));
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (allocator) {
		void *allocation = malloc(zloc__KILOBYTE(1));
		if (!zloc_Free(allocator, allocation)) {
			result = 1;
		}
		zloc_free_memory(allocation);
	}
	zloc_free_memory(memory);
	return result;
}

//Write outside the bounds of an allocation
int TestMemoryCorruptionDetection(void) {
	int result = 0;
	void *memory = malloc(zloc__MEGABYTE(1));
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, zloc__MEGABYTE(1));
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (allocator) {
		int *allocation = zloc_Allocate(allocator, sizeof(int) * 10);
		for (int i = 0; i != 20; ++i) {
			allocation[i] = rand();
		}
		if (!zloc_Free(allocator, allocation)) {
			result = 1;
		}
	}
	zloc_free_memory(memory);
	return result;
}

//Write outside the bounds of an allocation
int TestMemoryCorruptionDetection2(void) {
	int result = 0;
	void *memory = malloc(zloc__MEGABYTE(1));
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, zloc__MEGABYTE(1));
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (allocator) {
		int *allocation = zloc_Allocate(allocator, sizeof(int) * 10);
		for (int i = -5; i != 10; ++i) {
			allocation[i] = rand();
		}
		if (!zloc_Free(allocator, allocation)) {
			result = 1;
		}
	}
	zloc_free_memory(memory);
	return result;
}

int TestNonAlignedMemoryPool(void) {
	void *memory = malloc(1023);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, 1023);
	if (!allocator) {
		zloc_free_memory(memory);
		return 1;
	}
	zloc_free_memory(memory);
	return 0;
}

int TestAllocateSingleOverAllocate(void) {
	zloc_size size = zloc__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = zloc_Allocate(allocator, size);
		if (allocation) {
			result = 0;
		}
	}
	zloc_free_memory(memory);
	return result;
}

int TestAllocateMultiOverAllocate(void) {
	zloc_size size = zloc__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
	}
	zloc_size allocated = 0;
	while (zloc_Allocate(allocator, 1024)) {
		allocated += 1024;
		if (zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) != zloc__OK) {
			result = 0;
			break;
		}
		if (allocated > size) {
			result = 0;
			break;
		}
	}
	zloc_free_memory(memory);
	return result;
}

//This tests that free blocks in the segregated list will be exhausted first before using the main pool for allocations
int TestAllocateFreeSameSizeBlocks(void) {
	zloc_size size = zloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
	}
	void *allocations[40];
	for (int i = 0; i != 20; ++i) {
		allocations[i] = zloc_Allocate(allocator, 1024);
		if (!allocations[i]) {
			result = 0;
		}
	}
	//Free every second one so that blocks don't get merged
	for (int i = 0; i != 20; i += 2) {
		zloc_Free(allocator, allocations[i]);
		zloc__error_codes error = zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0);
		assert(error == zloc__OK);
		allocations[i] = 0;
	}
	for (int i = 0; i != 40; ++i) {
		allocations[i] = zloc_Allocate(allocator, 1024);
		zloc__error_codes error = zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0);
		assert(error == zloc__OK);
		if (!allocations[i]) {
			result = 0;
		}
	}
	for (int i = 0; i != 40; ++i) {
		if (allocations[i]) {
			zloc__error_codes error = zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0);
			assert(error == zloc__OK);
			zloc_Free(allocator, allocations[i]);
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
	}
	zloc_free_memory(memory);
	return result;
}

//Test allocating some memory that is too small
int TestAllocationTooSmall(void) {
	zloc_size size = zloc__MEGABYTE(2);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = zloc_Allocate(allocator, 4);
		if (zloc__block_size(zloc__block_from_allocation(allocation)) == zloc__MINIMUM_BLOCK_SIZE) {
			result = 1;
		}
		else {
			result = 0;
		}
	}
	zloc_free_memory(memory);
	return result;
}

int TestReAllocation(void) {
	zloc_size size = zloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = zloc_Allocate(allocator, 1024);
		if (!allocation) {
			result = 0;
		}
		else {
			allocation = zloc_Reallocate(allocator, allocation, 2048);
			if (!allocation) {
				result = 0;
			}
			else if (zloc__block_size(zloc__block_from_allocation(allocation)) != 2048) {
				result = 0;
			}
		}
	}
	zloc_free_memory(memory);
	return result;
}

int TestReAllocationFallbackToAllocateAndCopy(void) {
	zloc_size size = zloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation1 = zloc_Allocate(allocator, 1024);
		allocation1 = zloc_Reallocate(allocator, allocation1, 2048);
		if (!allocation1) {
			result = 0;
		}
	}
	zloc_free_memory(memory);
	return result;
}

int TestReAllocationOfNullPtr(void) {
	zloc_size size = zloc__MEGABYTE(16);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = 0;
		allocation = zloc_Reallocate(allocator, allocation, 1024);
		if (!allocation) {
			result = 0;
		}
		else {
			result = 1;
		}
	}
	zloc_free_memory(memory);
	return result;
}

int TestAlignedAllocation(void) {
	zloc_size size = zloc__MEGABYTE(1);
	int result = 1;
	void *memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	if (!allocator) {
		result = 0;
	}
	else {
		void *allocation = 0;
		allocation = zloc_AllocateAligned(allocator, 1024, 64);
		if (!allocation) {
			result = 0;
		}
		else {
			result = 1;
		}
	}
	zloc_free_memory(memory);
	return result;
}

int TestFreeAllBuffersAndPools(zloc_allocator *allocator, void *memory[8], void *buffers[100]) {
	int result = 1;

	for (int i = 0; i != 100; ++i) {
		if (buffers[i]) {
			if (!zloc_Free(allocator, buffers[i])) {
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
				zloc_VerifyBlocks(zloc__allocator_first_block(allocator), zloc__output, 0);
				result = zloc_RemovePool(allocator, zloc_GetPool(allocator));
			}
			else {
				zloc_VerifyBlocks(zloc__first_block_in_pool(memory[i]), zloc__output, 0);
				result = zloc_RemovePool(allocator, memory[i]);
			}
		}
	}

	for (int i = 0; i != 8; ++i) {
		if (memory[i]) {
			zloc_free_memory(memory[i]);
		}
	}

	return result;
}

int TestManyRandomAlignedAllocations(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	zloc_size alignments[7] = { 16, 32, 64, 128, 256, 512, 1024 };
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		//Do a bunch of normal allocations first
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				zloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//zloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				allocations[index] = zloc_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				zloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				int alignment = rand() % 7;
				allocations[index] = zloc_AllocateAligned(allocator, allocation_size, alignments[alignment]);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		zloc_free_memory(memory);
	}
	return result;
}

int TestManyAlignedAllocationsAndFreesAddPools(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory[8];
	memset(memory, 0, sizeof(void*) * 8);
	int memory_index = 0;
	memory[memory_index] = malloc(pool_size);
	memset(memory[memory_index], 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory[memory_index++], pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory[0]);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		zloc_size alignments[7] = { 16, 32, 64, 128, 256, 512, 1024 };
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				zloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//zloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				int alignment = rand() % 7;
				allocations[index] = zloc_AllocateAligned(allocator, allocation_size, alignments[alignment]);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
				else if (memory_index < 8) {
					//We ran out of memory, add a new pool
					memory[memory_index] = malloc(pool_size);
					if (memory[memory_index]) {
						zloc_AddPool(allocator, memory[memory_index++], pool_size);
						allocations[index] = zloc_AllocateAligned(allocator, allocation_size, alignments[alignment]);
						if (!allocations[index]) {
							result = 0;
							break;
						}
					}
				}
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		result = TestFreeAllBuffersAndPools(allocator, memory, allocations);
	}
	return result;
}

int TestManyAllocationsAndFreesDummy(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size) {
	int result = 1;
	zloc_size allocations[100];
	memset(allocations, 0, sizeof(zloc_size) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		if (allocations[index]) {
			allocations[index] = 0;
		}
		else {
			zloc_size allocation_size = (rand() % max_allocation_size) + zloc__MINIMUM_BLOCK_SIZE;
			allocations[index] = allocation_size;
		}
	}
	return result;
}

//Do lots of allocations and frees of random sizes
int TestManyAllocationsAndFrees(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				zloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//zloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				allocations[index] = zloc_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		zloc_free_memory(memory);
	}
	return result;
}

int TestManyAllocationsAndFreesAddPools(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory[8];
	memset(memory, 0, sizeof(void*) * 8);
	int memory_index = 0;
	memory[memory_index] = malloc(pool_size);
	memset(memory[memory_index], 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory[memory_index++], pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory[0]);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				zloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				//zloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
				allocations[index] = zloc_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
				else if(memory_index < 8) {
					//We ran out of memory, add a new pool
					memory[memory_index] = malloc(pool_size);
                    if(memory[memory_index]) {
                        zloc_AddPool(allocator, memory[memory_index++], pool_size);
                        allocations[index] = zloc_Allocate(allocator, allocation_size);
                        if (!allocations[index]) {
                            result = 0;
                            break;
                        }
                    }
				}
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		result = TestFreeAllBuffersAndPools(allocator, memory, allocations);
	}
	return result;
}

int TestAllocatingUntilOutOfSpaceThenRandomFreesAndAllocations(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = zloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 1000;
			if (allocations[index]) {
				zloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
				allocations[index] = zloc_Allocate(allocator, allocation_size);
				if (allocations[index]) {
					//Do a memset set to test if we're overwriting block headers
					memset(allocations[index], 7, allocation_size);
				}
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		zloc_free_memory(memory);
	}
	return result;
}

//Test that after allocating lots of allocations and freeing them all, there should only be one free allocation
//because all free blocks should get merged
int TestAllocatingUntilOutOfSpaceThenFreeAll(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = zloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				zloc_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		zloc_header *first_block = zloc__allocator_first_block(allocator);
		result = zloc__is_last_block_in_pool(zloc__next_physical_block(first_block));
		zloc_free_memory(memory);
	}
	return result;
}

int TestRemovingPool(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = zloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				zloc_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		if (!zloc_RemovePool(allocator, zloc_GetPool(allocator))) {
			result = 0;
		}
		zloc_free_memory(memory);
	}
	return result;
}

int TestRemovingExtraPool(zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	void *memory = malloc(pool_size);
	zloc_pool *extra_pool = 0;
	memset(memory, 0, pool_size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, pool_size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
		zloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			allocations[i] = zloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				void *extra_memory = malloc(pool_size);
				extra_pool = zloc_AddPool(allocator, extra_memory, pool_size);
				allocations[i] = zloc_Allocate(allocator, allocation_size);
				break;
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				zloc_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(zloc_VerifyBlocks(zloc__allocator_first_block(allocator), 0, 0) == zloc__OK);
		}
		if (!zloc_RemovePool(allocator, extra_pool)) {
			result = 0;
		}
		if (!zloc_RemovePool(allocator, zloc_GetPool(allocator))) {
			result = 0;
		}
		zloc_free_memory(memory);
		if (extra_pool) {
			zloc_free_memory(extra_pool);
		}
	}
	return result;
}

//64bit tests
#if defined(zloc__64BIT)
//Allocate a large block
int TestAllocation64bit(void) {
	zloc_size size = (1024ull * 1024ull * 1024ull * 6ull);	//6 gb
	int result = 1;
	void* memory = malloc(size);
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
	assert(zloc_VerifySegregatedLists(allocator) == zloc__OK);
	if (!allocator) {
		result = 0;
	}
	if(allocator) {
		if (zloc_Allocate(allocator, size / 2)) {
			result = 1;
		}
	}
	zloc_free_memory(memory);
	return result;
}
#endif

typedef struct zloc_thread_test {
	zloc_allocator *allocator;
	void *allocations[100];
	zloc_random *random;
	zloc_uint iterations;
	zloc_size pool_size;
	zloc_size min_allocation_size;
	zloc_size max_allocation_size;
	zloc_index memory_index;
} zloc_thread_test;

#ifdef ZLOC_THREAD_SAFE
struct zloc_memory_threads {
	void *memory[9];
	volatile zloc_thread_access access;
};

struct zloc_memory_threads thread_memory;

// Function that will be executed by the thread
void *AllocationWorker(void *arg) {
	zloc_thread_test *thread_test = (zloc_thread_test*)arg;
	for (int i = 0; i != thread_test->iterations; ++i) {
		int index = rand() % 100;
		if (thread_test->allocations[index]) {
			zloc_Free(thread_test->allocator, thread_test->allocations[index]);
			thread_test->allocations[index] = 0;
		}
		else {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(thread_test->random, thread_test->max_allocation_size - thread_test->min_allocation_size) + thread_test->min_allocation_size;
			thread_test->allocations[index] = zloc_Allocate(thread_test->allocator, allocation_size);
			if (thread_test->allocations[index]) {
				//Do a memset set to test if we're overwriting block headers
				memset(thread_test->allocations[index], 7, allocation_size);
			}
		}
	}
	return 0;
}

void *AllocationWorkerAddPool(void *arg) {
	zloc_thread_test *thread_test = (zloc_thread_test*)arg;
	for (int i = 0; i != thread_test->iterations; ++i) {
		int index = rand() % 100;
		if (thread_test->allocations[index]) {
			zloc_Free(thread_test->allocator, thread_test->allocations[index]);
			thread_test->allocations[index] = 0;
		}
		else {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(thread_test->random, thread_test->max_allocation_size - thread_test->min_allocation_size) + thread_test->min_allocation_size;
			thread_test->allocations[index] = zloc_Allocate(thread_test->allocator, allocation_size);
			if (thread_test->allocations[index]) {
				//Do a memset set to test if we're overwriting block headers
				memset(thread_test->allocations[index], 7, allocation_size);
			}
			else if (thread_memory.memory[thread_test->memory_index] == 0) {
				//We ran out of memory, add a new pool
				zloc_thread_access original_access = thread_memory.access;
				zloc_thread_access access = zloc__compare_and_exchange(&thread_memory.access, 1, original_access);
				if (original_access == access) {
					zloc_index index = thread_test->memory_index;
					thread_memory.memory[index] = malloc(thread_test->pool_size);
					zloc_AddPool(thread_test->allocator, thread_memory.memory[index], thread_test->pool_size);
					printf("\033[34mThread %i added pool\033[0m\n", index);
					thread_test->allocations[index] = zloc_Allocate(thread_test->allocator, allocation_size);
					thread_memory.access = 0;
				}
				else {
					do {} while (thread_memory.access == 1);
					thread_test->allocations[index] = zloc_Allocate(thread_test->allocator, allocation_size);
				}
			}
		}
	}
	return 0;
}

int TestMultithreading(zloc__allocation_thread callback, zloc_uint iterations, zloc_size pool_size, zloc_size min_allocation_size, zloc_size max_allocation_size, int thread_count, zloc_random *random) {
	int result = 1;

	thread_memory.memory[0] = malloc(pool_size);
	zloc_thread_test thread[8];
	zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(thread_memory.memory[0], pool_size);
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

	zloc_index index = 0;
	while (thread_memory.memory[index]) {
		zloc_free_memory(thread_memory.memory[index]);
		index++;
	}

	return 1;
}
#endif

#ifdef ZLOC_ENABLE_REMOTE_MEMORY
//Test remote pools
typedef struct remote_memory_pools {
	void *memory_pools[8];
	void *range_pools[8];
	zloc_size pool_sizes[8];
	zloc_uint pool_count;
} remote_memory_pools;

typedef struct remote_buffer {
	zloc_size size;
	zloc_size offset_from_pool;
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

void on_split_block(void *user_data, zloc_header* block, zloc_header *trimmed_block, zloc_size remote_size) {
	remote_buffer *buffer = zloc_BlockUserExtensionPtr(block);
	remote_buffer *trimmed_buffer = zloc_BlockUserExtensionPtr(trimmed_block);
	trimmed_buffer->size = buffer->size - remote_size;
	buffer->size = remote_size;
	trimmed_buffer->pool = buffer->pool;
	trimmed_buffer->offset_from_pool = buffer->offset_from_pool + buffer->size;
	buffer->data = (void*)((char*)buffer->pool + buffer->offset_from_pool);
}

void on_reallocation_copy(void *user_data, zloc_header* block, zloc_header *new_block) {
	remote_buffer *buffer = zloc_BlockUserExtensionPtr(block);
	remote_buffer *new_buffer = zloc_BlockUserExtensionPtr(new_block);
	new_buffer->data = (void*)((char*)new_buffer->pool + new_buffer->offset_from_pool);
	memcpy(new_buffer->data, buffer->data, buffer->size);
}

static void zloc__output_buffer_info(void* ptr, size_t size, int free, void* user, int count)
{
	remote_buffer *buffer = (remote_buffer*)user;
	printf("%i) \t%s size: \t%zi \tbuffer size: %zu \toffset: %zu \n", count, free ? "free" : "used", size, buffer->size, buffer->offset_from_pool);
}

int TestFreeAllRemoteBuffersAndPools(zloc_allocator *allocator, remote_memory_pools *pools, remote_buffer *buffers[100]) {
	int result = 1;
	for (int i = 0; i != 100; ++i) {
		if (buffers[i]) {
			if (!zloc_FreeRemote(allocator, buffers[i])) {
				result = 0;
				break;
			}
			else {
				buffers[i] = 0;
			}
		}
	}
	for (int i = 0; i != pools->pool_count; ++i) {
		zloc_VerifyRemoteBlocks(zloc__first_block_in_pool(pools->range_pools[i]), zloc__output_buffer_info, 0);
		result = zloc_RemovePool(allocator, pools->range_pools[i]);
		zloc_free_memory(pools->range_pools[i]);
		zloc_free_memory(pools->memory_pools[i]);
	}

	return result;
}

int TestRemoteMemoryBlockManagement(zloc_uint iterations, zloc_size pool_size, zloc_size minimum_remote_allocation_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	zloc_allocator *allocator;
	void *allocator_memory = malloc(zloc_AllocatorSize());
	allocator = zloc_InitialiseAllocatorForRemote(allocator_memory);
	zloc_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	zloc_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	//zloc_size memory_sizes[4] = { zloc__MEGABYTE(1), zloc__MEGABYTE(2), zloc__MEGABYTE(3), zloc__MEGABYTE(4) };
	zloc_size range_pool_size = zloc_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	zloc_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		if (buffers[index]) {
			if (!zloc_FreeRemote(allocator, buffers[index])) {
				result = 0;
				break;
			}
			buffers[index] = 0;
		}
		else {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			//zloc_size allocation_size = memory_sizes[rand() % 4];
			buffers[index] = zloc_AllocateRemote(allocator, allocation_size);
			if (!buffers[index]) {
				//Ran out of room in the pool
				if (pools.pool_count == 8) {
					continue;
				}
				pools.pool_sizes[pools.pool_count] = pool_size;
				range_pool_size = zloc_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
				pools.range_pools[pools.pool_count] = malloc(range_pool_size);
				pools.memory_pools[pools.pool_count] = malloc(pool_size);
				zloc_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
				buffers[index] = zloc_AllocateRemote(allocator, allocation_size);
			}
			else {
				buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
			}
		}
		for (int c = 0; c != pools.pool_count; ++c) {
			assert(zloc_VerifyRemoteBlocks(zloc__first_block_in_pool(pools.range_pools[c]), 0, 0) == zloc__OK);
		}
	}
	//remote_buffer *test = zloc_AllocateRemote(allocator, zloc__MEGABYTE(32));
	TestFreeAllRemoteBuffersAndPools(allocator, &pools, buffers);
	zloc_free_memory(allocator_memory);
	return result;
}

int TestRemoteMemoryReallocation(zloc_uint iterations, zloc_size pool_size, zloc_size minimum_remote_allocation_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	zloc_allocator *allocator;
	void *allocator_memory = malloc(zloc_AllocatorSize());
	allocator = zloc_InitialiseAllocatorForRemote(allocator_memory);
	zloc_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	zloc_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	//zloc_size memory_sizes[4] = { zloc__MEGABYTE(1), zloc__MEGABYTE(2), zloc__MEGABYTE(3), zloc__MEGABYTE(4) };
	zloc_size range_pool_size = zloc_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	zloc_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	buffers[0] = zloc_ReallocateRemote(allocator, buffers[0], zloc__KILOBYTE(1));
	buffers[0]->data = (void*)((char*)pools.memory_pools[0] + buffers[0]->offset_from_pool);
	buffers[1] = zloc_ReallocateRemote(allocator, buffers[1], zloc__KILOBYTE(1));
	buffers[1]->data = (void*)((char*)pools.memory_pools[0] + buffers[1]->offset_from_pool);
	buffers[0] = zloc_ReallocateRemote(allocator, buffers[0], zloc__KILOBYTE(2));
	buffers[0]->data = (void*)((char*)pools.memory_pools[0] + buffers[0]->offset_from_pool);
	buffers[1] = zloc_ReallocateRemote(allocator, buffers[1], zloc__KILOBYTE(2));
	buffers[1]->data = (void*)((char*)pools.memory_pools[0] + buffers[1]->offset_from_pool);
	//remote_buffer *test = zloc_AllocateRemote(allocator, zloc__MEGABYTE(32));
	for (int c = 0; c != pools.pool_count; ++c) {
		zloc_VerifyRemoteBlocks(zloc__first_block_in_pool(pools.range_pools[c]), zloc__output_buffer_info, 0);
		zloc_free_memory(pools.range_pools[c]);
		zloc_free_memory(pools.memory_pools[c]);
	}
	zloc_free_memory(allocator_memory);
	return result;
}

int TestRemoteMemoryReallocationIterations(zloc_uint iterations, zloc_size pool_size, zloc_size minimum_remote_allocation_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	zloc_allocator *allocator;
	void *allocator_memory = malloc(zloc_AllocatorSize());
	allocator = zloc_InitialiseAllocatorForRemote(allocator_memory);
	zloc_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	zloc_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	zloc_size range_pool_size = zloc_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	zloc_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
		buffers[index] = zloc_ReallocateRemote(allocator, buffers[index], allocation_size);
		if (!buffers[index]) {
			//Ran out of room in the pool
			if (pools.pool_count == 8) {
				continue;
			}
			pools.pool_sizes[pools.pool_count] = pool_size;
			range_pool_size = zloc_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
			pools.range_pools[pools.pool_count] = malloc(range_pool_size);
			pools.memory_pools[pools.pool_count] = malloc(pool_size);
			zloc_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
			buffers[index] = zloc_ReallocateRemote(allocator, buffers[index], allocation_size);
			if (buffers[index]) {
				buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
			}
		}
		else {
			buffers[index]->data = (void*)((char*)buffers[index]->pool + buffers[index]->offset_from_pool);
		}
		for (int c = 0; c != pools.pool_count; ++c) {
			assert(zloc_VerifyRemoteBlocks(zloc__first_block_in_pool(pools.range_pools[c]), 0, 0) == zloc__OK);
		}
	}
	//remote_buffer *test = zloc_AllocateRemote(allocator, zloc__MEGABYTE(32));
	for (int c = 0; c != pools.pool_count; ++c) {
		//zloc_VerifyRemoteBlocks(zloc__first_block_in_pool(pools.range_pools[c]), zloc__output_buffer_info, 0);
		zloc_free_memory(pools.range_pools[c]);
		zloc_free_memory(pools.memory_pools[c]);
	}
	zloc_free_memory(allocator_memory);
	return result;
}

int TestRemoteMemoryReallocationIterationsFreeing(zloc_uint iterations, zloc_size pool_size, zloc_size minimum_remote_allocation_size, zloc_size min_allocation_size, zloc_size max_allocation_size, zloc_random *random) {
	int result = 1;
	remote_memory_pools pools;
	pools.pool_sizes[0] = pool_size;
	pools.pool_count = 0;
	zloc_allocator *allocator;
	void *allocator_memory = malloc(zloc_AllocatorSize());
	allocator = zloc_InitialiseAllocatorForRemote(allocator_memory);
	zloc_SetBlockExtensionSize(allocator, sizeof(remote_buffer));
	zloc_SetMinimumAllocationSize(allocator, minimum_remote_allocation_size);
	allocator->user_data = &pools;
	allocator->add_pool_callback = on_add_pool;
	allocator->split_block_callback = on_split_block;
	allocator->unable_to_reallocate_callback = on_reallocation_copy;
	zloc_size range_pool_size = zloc_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
	pools.range_pools[pools.pool_count] = malloc(range_pool_size);
	pools.memory_pools[pools.pool_count] = malloc(pool_size);
	zloc_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
	remote_buffer *buffers[100];
	memset(buffers, 0, sizeof(void*) * 100);
	for (int i = 0; i != iterations; ++i) {
		int index = rand() % 100;
		if (buffers[index] && buffers[index]->size > min_allocation_size + ((max_allocation_size - min_allocation_size) / 2)) {
			if (!zloc_FreeRemote(allocator, buffers[index])) {
				result = 0;
				break;
			}
			buffers[index] = 0;
		}
		else if(!buffers[index]) {
			zloc_size allocation_size = (zloc_size)_zloc_random_range(random, max_allocation_size - min_allocation_size) + min_allocation_size;
			//zloc_size allocation_size = memory_sizes[rand() % 4];
			remote_buffer *new_allocation = zloc_ReallocateRemote(allocator, buffers[index], allocation_size);
			if (!new_allocation) {
				//Ran out of room in the pool
				if (pools.pool_count == 8) {
					continue;
				}
				pools.pool_sizes[pools.pool_count] = pool_size;
				range_pool_size = zloc_CalculateRemoteBlockPoolSize(allocator, pools.pool_sizes[pools.pool_count]);
				pools.range_pools[pools.pool_count] = malloc(range_pool_size);
				pools.memory_pools[pools.pool_count] = malloc(pool_size);
				zloc_AddRemotePool(allocator, pools.range_pools[pools.pool_count], range_pool_size, pools.pool_sizes[pools.pool_count]);
				new_allocation = zloc_ReallocateRemote(allocator, buffers[index], allocation_size);
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
			assert(zloc_VerifyRemoteBlocks(zloc__first_block_in_pool(pools.range_pools[c]), 0, 0) == zloc__OK);
		}
	}
	result = TestFreeAllRemoteBuffersAndPools(allocator, &pools, buffers);
	zloc_free_memory(allocator_memory);
	return result;
}
#endif

int main() {

	zloc_random random;
	zloc_size time = (zloc_size)clock() * 1000;
	_ReSeed(&random, time);
	_ReSeed(&random, 180000);
	//_ReSeed(&random, 123456);

#if defined(ZLOC_THREAD_SAFE)
	PrintTestResult("Test: Multithreading test, 2 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, zloc__MEGABYTE(128), zloc__MINIMUM_BLOCK_SIZE, zloc__KILOBYTE(256), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, zloc__MEGABYTE(128), zloc__MINIMUM_BLOCK_SIZE, zloc__KILOBYTE(256), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers, 1000 iterations of allocating and freeing 16b-256kb in a 128MB pool", TestMultithreading(AllocationWorker, 1000, zloc__MEGABYTE(128), zloc__MINIMUM_BLOCK_SIZE, zloc__KILOBYTE(256), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(1), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(1), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers, 1000 iterations of allocating and freeing 16b-1mb in a 256MB pool", TestMultithreading(AllocationWorker, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(1), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(2), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(2), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers add pool if needed, 1000 iterations of allocating and freeing 16b-2mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(2), 8, &random));
	PrintTestResult("Test: Multithreading test, 2 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(10), 2, &random));
	PrintTestResult("Test: Multithreading test, 4 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(10), 4, &random));
	PrintTestResult("Test: Multithreading test, 8 workers add pool if needed, 1000 iterations of allocating and freeing 16b-10mb in a 256MB pools", TestMultithreading(AllocationWorkerAddPool, 1000, zloc__MEGABYTE(256), zloc__MINIMUM_BLOCK_SIZE, zloc__MEGABYTE(10), 8, &random));
#endif
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 16b - 256kb", TestManyAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), zloc__MINIMUM_BLOCK_SIZE, zloc__KILOBYTE(256), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), zloc__MEGABYTE(2), zloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks one by one resulting in 1 block left at the end after merges", TestAllocatingUntilOutOfSpaceThenFreeAll(1000, zloc__MEGABYTE(128), zloc__KILOBYTE(128), zloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks and remove the pool", TestRemovingPool(1000, zloc__MEGABYTE(128), zloc__KILOBYTE(128), zloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks and remove the pool", TestRemovingExtraPool(1000, zloc__MEGABYTE(128), zloc__KILOBYTE(128), zloc__MEGABYTE(10), &random));
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
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 16b - 256kb", TestManyAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), zloc__MINIMUM_BLOCK_SIZE, zloc__KILOBYTE(256), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 64kb - 1MB", TestManyAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), 64 * 1024, zloc__MEGABYTE(1), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 1MB - 2MB", TestManyAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), zloc__MEGABYTE(1), zloc__MEGABYTE(2), &random));
	PrintTestResult("Test: Many random allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), zloc__MEGABYTE(2), zloc__MEGABYTE(10), &random));
	PrintTestResult("Test: Many random allocations and frees, go oom: 1000 iterations, 1GB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, zloc__GIGABYTE(1), zloc__KILOBYTE(256), zloc__MEGABYTE(50), &random));
	PrintTestResult("Test: Many random allocations and frees, go oom: 1000 iterations, 512MB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, zloc__MEGABYTE(512), zloc__KILOBYTE(256), zloc__MEGABYTE(25), &random));
	PrintTestResult("Test: Single aligned allocation", TestAlignedAllocation());
	PrintTestResult("Test: Many random aligned allocations and frees 1000 iterations, 128MB pool size, max allocation: 256b - 2mb", TestManyRandomAlignedAllocations(1000, zloc__MEGABYTE(128), 256, zloc__MEGABYTE(2), &random));
	PrintTestResult("Test: Many random aligned allocations and frees 1000 iterations, 128MB pool size, max allocation: 2kb - 4mb", TestManyRandomAlignedAllocations(1000, zloc__MEGABYTE(128), zloc__KILOBYTE(2), zloc__MEGABYTE(4), &random));
	PrintTestResult("Test: Many random aligned allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 64kb - 1MB", TestManyAlignedAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), 64 * 1024, zloc__MEGABYTE(1), &random));
	PrintTestResult("Test: Many random aligned allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 1MB - 2MB", TestManyAlignedAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), zloc__MEGABYTE(1), zloc__MEGABYTE(2), &random));
	PrintTestResult("Test: Many random aligned allocations and frees, add pools as needed: 1000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAlignedAllocationsAndFreesAddPools(1000, zloc__MEGABYTE(128), zloc__MEGABYTE(2), zloc__MEGABYTE(10), &random));
#if defined(zloc__64BIT)
	PrintTestResult("Test: Create a large (>4gb) memory pool, and allocate half of it", TestAllocation64bit());
#endif

#ifdef ZLOC_ENABLE_REMOTE_MEMORY
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 16b - 1k, add 1mb pools as needed.", TestRemoteMemoryBlockManagement(10000, zloc__MEGABYTE(1), 512, 16, zloc__KILOBYTE(1), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 8kb - 64kb, add 16mb pools as needed.", TestRemoteMemoryBlockManagement(10000, zloc__MEGABYTE(64), zloc__KILOBYTE(8), zloc__KILOBYTE(8), zloc__KILOBYTE(64), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 256kb - 2mb, add 64mb pools as needed.", TestRemoteMemoryBlockManagement(10000, zloc__MEGABYTE(64), zloc__KILOBYTE(256), zloc__KILOBYTE(256), zloc__MEGABYTE(2), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 1MB - 64mb, add 128mb pools as needed.", TestRemoteMemoryBlockManagement(10000, zloc__MEGABYTE(128), zloc__MEGABYTE(1), zloc__MEGABYTE(1), zloc__MEGABYTE(64), &random));
	PrintTestResult("Test: Remote memory management, Reallocation", TestRemoteMemoryReallocation(10000, zloc__MEGABYTE(16), 512, 16, zloc__KILOBYTE(1), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 512b - 4kb", TestRemoteMemoryReallocationIterations(10000, zloc__MEGABYTE(16), 512, 512, zloc__KILOBYTE(4), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 256kb - 2MB", TestRemoteMemoryReallocationIterations(10000, zloc__MEGABYTE(16), zloc__KILOBYTE(256), zloc__KILOBYTE(256), zloc__MEGABYTE(2), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 256kb - 4MB", TestRemoteMemoryReallocationIterations(10000, zloc__MEGABYTE(64), zloc__KILOBYTE(256), zloc__KILOBYTE(256), zloc__MEGABYTE(4), &random));
	PrintTestResult("Test: Remote memory management, Reallocation until full 10000 iterations 256kb - 4MB with Freeing", TestRemoteMemoryReallocationIterationsFreeing(10000, zloc__MEGABYTE(64), zloc__KILOBYTE(256), zloc__KILOBYTE(256), zloc__MEGABYTE(4), &random));
	PrintTestResult("Test: Remote memory management, 10000 iterations, allocate 1MB - 64mb, add 128mb pools as needed.", TestRemoteMemoryReallocationIterationsFreeing(10000, zloc__MEGABYTE(128), zloc__MEGABYTE(1), zloc__MEGABYTE(1), zloc__MEGABYTE(16), &random));
#endif
	return 0;
}

#endif
