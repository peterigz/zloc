//#define TLOC_DEV_MODE
#define TLOC_IMPLEMENTATION
#define TLOC_OUTPUT_ERROR_MESSAGES

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
	TLoc *allocator = tloc_InitialiseAllocator(memory, 1024, 5);
	if (!allocator) {
		tloc_free_memory(memory);
		return 1;
	}
	tloc_free_memory(memory);
	return 0;
}

int TestNonAlignedMemoryPool() {
	void *memory = malloc(1023);
	TLoc *allocator = tloc_InitialiseAllocator(memory, 1023, 5);
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
	TLoc *allocator = tloc_InitialiseAllocator(memory, size, 5);
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
	TLoc *allocator = tloc_InitialiseAllocator(memory, size, 5);
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

//Test allocating some memory that is too small
int TestAllocationTooSmall() {
	tloc_size size = 1024 * 1024;
	int result = 1;
	void *memory = malloc(size);
	TLoc *allocator = tloc_InitialiseAllocator(memory, size, 5);
	if (!allocator) {
		result = 0;
	}
	if (tloc_Allocate(allocator, 4)) {
		result = 0;
	}
	tloc_free_memory(memory);
	return result;
}

//Allocate a large block
int TestAllocation64bit() {
	tloc_size size = (1024ull * 1024ull * 1024ull * 6ull);	//6 gb
	int result = 1;
	void* memory = malloc(size);
	TLoc *allocator = tloc_InitialiseAllocator(memory, size, 5);
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

int main()
{
	PrintTestResult("Test: Pool passed to initialiser is too small", TestPoolTooSmall());
	PrintTestResult("Test: Non aligned memory pool passed to Initialiser", TestNonAlignedMemoryPool());
	PrintTestResult("Test: Attempt to allocate more memory than is available in one go", TestAllocateSingleOverAllocate());
	PrintTestResult("Test: Attempt to allocate more memory than is available with multiple attemps", TestAllocateMultiOverAllocate());
	PrintTestResult("Test: Attempt to allocate memory that is below minimum block size", TestAllocationTooSmall());
	PrintTestResult("Test: Create a large (>2gb) memory pool, and allocate half of it. Note: Failed may mean unable to malloc >2gb", TestAllocation64bit());
}

#endif
