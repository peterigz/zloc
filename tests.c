#define TLOC_ERROR_COLOR "\033[90m"
#define TLOC_IMPLEMENTATION
#define TLOC_OUTPUT_ERROR_MESSAGES
#include <stdio.h>

#if !defined(TLOC_DEV_MODE)

#include "2loc.h"

#define tloc_free_memory(memory) if(memory) free(memory);

void PrintTestResult(const char *message, int result) {
	printf("%s", message);
	printf("%s [%s]\033[0m\n", result == 0 ? "\033[31m" : "\033[32m", result == 0 ? "FAILED" : "PASSED");
}

//Test helper function calculates the correct size
int TestSizeHelperFunction(tloc_size size) {
	tloc_size calculated_size = tloc_CalculateAllocationSize(size);
	void *memory = malloc(calculated_size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, calculated_size);
	if (allocator) {
		tloc_size pool_size = tloc_AllocatorPoolSize(allocator);
		if (pool_size == size) {
			return 1;
		}
	}
	tloc_free_memory(memory);
	return 0;
}

//Test if passing a memory pool that is too small to the initialiser is handled gracefully
int TestPoolTooSmall() {
	void *memory = malloc(1024);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, 1024);
	if (!allocator) {
		tloc_free_memory(memory);
		return 1;
	}
	tloc_free_memory(memory);
	return 0;
}

//Test if trying to free an invalid memory block fails gracefully
int TestFreeingAnInvalidAllocation() {
	int result = 0;
	void *memory = malloc(tloc__MEGABYTE(1));
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, tloc__MEGABYTE(1));
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
int TestMemoryCorruptionDetection() {
	int result = 0;
	void *memory = malloc(tloc__MEGABYTE(1));
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, tloc__MEGABYTE(1));
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
int TestMemoryCorruptionDetection2() {
	int result = 0;
	void *memory = malloc(tloc__MEGABYTE(1));
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, tloc__MEGABYTE(1));
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

int TestNonAlignedMemoryPool() {
	void *memory = malloc(1023);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, 1023);
	if (!allocator) {
		tloc_free_memory(memory);
		return 1;
	}
	tloc_free_memory(memory);
	return 0;
}

int TestAllocateSingleOverAllocate() {
	tloc_size size = 1024 * 1024;
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
	if (!allocator) {
		result = 0;
	}
	void *allocation = tloc_Allocate(allocator, size);
	if (allocation) {
		result = 0;
	}
	tloc_free_memory(memory);
	return result;
}

int TestAllocateMultiOverAllocate() {
	tloc_size size = 1024 * 1024;
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
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
int TestAllocateFreeSameSizeBlocks() {
	tloc_size size = 1024 * 1024 * 16;
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
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
		allocations[i] = 0;
	}
	for (int i = 0; i != 40; ++i) {
		allocations[i] = tloc_Allocate(allocator, 1024);
		if (!allocations[i]) {
			result = 0;
		}
	}
	for (int i = 0; i != 40; ++i) {
		if (allocations[i]) {
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
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
int TestAllocationTooSmall() {
	tloc_size size = 1024 * 1024;
	int result = 1;
	void *memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
	if (!allocator) {
		result = 0;
	}
	else if (tloc_Allocate(allocator, 4)) {
		result = 0;
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
int TestManyAllocationsAndFrees(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, pool_size);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[100];
		memset(allocations, 0, sizeof(void*) * 100);
		for (int i = 0; i != iterations; ++i) {
			int index = rand() % 100;
			if (allocations[index]) {
				tloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				tloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
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

int TestAllocatingUntilOutOfSpaceThenRandomFreesAndAllocations(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, pool_size);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			tloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
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
				tloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
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
int TestAllocatingUntilOutOfSpaceThenFreeAll(tloc_uint iterations, tloc_size pool_size, tloc_size min_allocation_size, tloc_size max_allocation_size) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, pool_size);
	if (!allocator) {
		result = 0;
		tloc_free_memory(memory);
	}
	else {
		void *allocations[1000];
		memset(allocations, 0, sizeof(void*) * 1000);
		for (int i = 0; i != 1000; ++i) {
			tloc_size allocation_size = (rand() % max_allocation_size) + min_allocation_size;
			allocations[i] = tloc_Allocate(allocator, allocation_size);
			if (!allocations[i]) {
				break;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		int count = tloc_BlockCount(allocator);
		for (int i = 0; i != 1000; ++i) {
			if (allocations[i]) {
				tloc_Free(allocator, allocations[i]);
				allocations[i] = 0;
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
		result = tloc_BlockCount(allocator) == 1 ? 1 : 0;
		tloc_free_memory(memory);
	}
	return result;
}

//64bit tests
#if defined(tloc__64BIT)
//Allocate a large block
int TestAllocation64bit() {
	tloc_size size = (1024ull * 1024ull * 1024ull * 6ull);	//6 gb
	int result = 1;
	void* memory = malloc(size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size);
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

int main() {
	PrintTestResult("Test: Helper function calculate allocation size 1MB and compare with actual allocated size", TestSizeHelperFunction(tloc__MEGABYTE(1)));
	PrintTestResult("Test: Helper function calculate allocation size 128MB and compare with actual allocated size", TestSizeHelperFunction(tloc__MEGABYTE(128)));
	PrintTestResult("Test: Helper function calculate allocation size 1GB and compare with actual allocated size", TestSizeHelperFunction(tloc__GIGABYTE(1)));
	PrintTestResult("Test: Pool passed to initialiser is too small", TestPoolTooSmall());
	PrintTestResult("Test: Non aligned memory passed to Initialiser", TestNonAlignedMemoryPool());
	PrintTestResult("Test: Attempt to allocate more memory than is available in one go", TestAllocateSingleOverAllocate());
	PrintTestResult("Test: Attempt to allocate more memory than is available with multiple attempts", TestAllocateMultiOverAllocate());
	PrintTestResult("Test: Attempt to allocate memory that is below minimum block size", TestAllocationTooSmall());
	PrintTestResult("Test: Multiple same size block allocations and frees", TestAllocateFreeSameSizeBlocks());
	PrintTestResult("Test: Try to free an invalid allocation address", TestFreeingAnInvalidAllocation());
	PrintTestResult("Test: Detect memory corruption by writing outside of bounds of an allocation (after)", TestMemoryCorruptionDetection());
	PrintTestResult("Test: Detect memory corruption by writing outside of bounds of an allocation (before)", TestMemoryCorruptionDetection2());
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 128MB pool size, max allocation: 16b - 256kb", TestManyAllocationsAndFrees(1000, tloc__MEGABYTE(128), tloc__MINIMUM_BLOCK_SIZE, tloc__KILOBYTE(256)));
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 128MB pool size, max allocation: 64kb - 1MB", TestManyAllocationsAndFrees(1000, tloc__MEGABYTE(128), 64 * 1024, tloc__MEGABYTE(1)));
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 128MB pool size, max allocation: 1MB - 2MB", TestManyAllocationsAndFrees(1000, tloc__MEGABYTE(128), tloc__MEGABYTE(1), tloc__MEGABYTE(2)));
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 128MB pool size, max allocation: 2MB - 10MB", TestManyAllocationsAndFrees(1000, tloc__MEGABYTE(128), tloc__MEGABYTE(2), tloc__MEGABYTE(10)));
#if defined(tloc__64BIT)
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 1GB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, tloc__GIGABYTE(1), tloc__MEGABYTE(2), tloc__MEGABYTE(100)));
#else
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 512MB pool size, max allocation: 2MB - 100MB", TestManyAllocationsAndFrees(1000, tloc__MEGABYTE(512), tloc__MEGABYTE(2), tloc__MEGABYTE(100)));
#endif
	PrintTestResult("Test: Allocations until full, then free and allocate randomly for 10000 iterations, 128MB pool size, max allocation: 128kb - 10MB", TestAllocatingUntilOutOfSpaceThenRandomFreesAndAllocations(1000, tloc__MEGABYTE(128), tloc__KILOBYTE(128), tloc__MEGABYTE(10)));
	PrintTestResult("Test: Allocate blocks in 128mb pool until full, then free all blocks one by one resulting in 1 block left at the end after merges", TestAllocatingUntilOutOfSpaceThenFreeAll(1000, tloc__MEGABYTE(128), tloc__KILOBYTE(128), tloc__MEGABYTE(10)));
#if defined(tloc__64BIT)
	//PrintTestResult("Test: Create a large (>4gb) memory pool, and allocate half of it", TestAllocation64bit());
#endif
}

#endif
