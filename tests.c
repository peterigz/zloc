//#define TLOC_DEV_MODE
#define TLOC_IMPLEMENTATION
#define TLOC_OUTPUT_ERROR_MESSAGES
//#define TLOC_OUTPUT_LOG_MESSAGES

#if !defined(TLOC_DEV_MODE)

#include "2loc.h"

#define tloc_free_memory(memory) if(memory) free(memory);

void PrintTestResult(const char *message, int result) {
	printf("%s", message);
	printf(" [%s]\n", result == 0 ? "FAILED" : "PASSED");
}

//Test if passing a memory pool that is too small to the initialiser is handled gracefully
int TestPoolTooSmall() {
	void *memory = malloc(1024);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, 1024, 5);
	if (!allocator) {
		tloc_free_memory(memory);
		return 1;
	}
	tloc_free_memory(memory);
	return 0;
}

int TestNonAlignedMemoryPool() {
	void *memory = malloc(1023);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, 1023, 5);
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
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size, 5);
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
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size, 5);
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
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size, 5);
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
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size, 5);
	if (!allocator) {
		result = 0;
	}
	else if (tloc_Allocate(allocator, 4)) {
		result = 0;
	}
	tloc_free_memory(memory);
	return result;
}

//Test that after allocating lots of allocations and freeing them all, there should only be one free allocation
//because all free blocks should get merged

int TestManyAllocationsAndFreesDummy(tloc_uint iterations, tloc_size pool_size, tloc_size max_allocation_size) {
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
int TestManyAllocationsAndFrees(tloc_uint iterations, tloc_size pool_size, tloc_size max_allocation_size) {
	int result = 1;
	void *memory = malloc(pool_size);
	memset(memory, 0, pool_size);
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, pool_size, 5);
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
				if (i == 812) {
					assert(tloc_CheckForNullBlocksInList(allocator));
				}
				tloc_Free(allocator, allocations[index]);
				allocations[index] = 0;
			}
			else {
				tloc_size allocation_size = (rand() % max_allocation_size) + tloc__MINIMUM_BLOCK_SIZE;
				if (i == 874) {
					tloc_bool null_blocks = tloc_CheckForNullBlocksInList(allocator);
					if (tloc_VerifyBlocks(allocator, 0, 0) != tloc__OK) {
						int d = 0;
					}
				}
				allocations[index] = tloc_Allocate(allocator, allocation_size);
			}
			assert(tloc_CheckForNullBlocksInList(allocator));
			assert(tloc_VerifyBlocks(allocator, 0, 0) == tloc__OK);
		}
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
	tloc_allocator *allocator = tloc_InitialiseAllocator(memory, size, 5);
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

int main()
{
	//These tests should be caught by the allocator and give a nice error message
	PrintTestResult("Test: Pool passed to initialiser is too small", TestPoolTooSmall());
	PrintTestResult("Test: Non aligned memory pool passed to Initialiser", TestNonAlignedMemoryPool());
	PrintTestResult("Test: Attempt to allocate more memory than is available in one go", TestAllocateSingleOverAllocate());
	PrintTestResult("Test: Attempt to allocate more memory than is available with multiple attemps", TestAllocateMultiOverAllocate());
	PrintTestResult("Test: Attempt to allocate memory that is below minimum block size", TestAllocationTooSmall());
	PrintTestResult("Test: Multiple same size block allocations and frees", TestAllocateFreeSameSizeBlocks());
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 1GB pool size, max allocation: 256kb", TestManyAllocationsAndFrees(1000, (1024ull * 1024ull * 1024ull * 1ull), 256 * 1024));
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 1GB pool size, max allocation: 1MB", TestManyAllocationsAndFrees(1000, (1024ull * 1024ull * 1024ull * 1ull), 1 * 1024 * 1024));
	PrintTestResult("Test: Many random allocations and frees: 10000 iterations, 1GB pool size, max allocation: 2MB", TestManyAllocationsAndFrees(1000, (1024ull * 1024ull * 1024ull * 1ull), 2 * 1024 * 1024));
#if defined(tloc__64BIT)
	//PrintTestResult("Test: Create a large (>4gb) memory pool, and allocate half of it", TestAllocation64bit());
#endif
}

#endif
