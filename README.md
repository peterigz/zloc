# Zest Allocator - A Two Level Segregated Fit Memory Allocator

This software is dual-licensed to the public domain and under the following license: you are granted a perpetual, irrevocable license to copy, modify, publish, and distribute this file as you see fit. See LICENSE file for details.

This library is a single header mimimal allocator based on the following paper: http://www.gii.upv.es/tlsf/files/ecrts04_tlsf.pdf

Thanks to the authors of the paper and also Sean Barret for his how to make a single header-file library guidelines, and also to Matthew Conte who's own TLSF lib I referenced when trying to understand how the algorythm works. His library can be found here: https://github.com/mattconte/tlsf

## What's this library for?
This library is for sub allocating memory blocks within larger memory allocation pools that you might create with malloc or VirtualAlloc etc.

Allocating and freeing those memory blocks happens at O(1) time complexity and should for the most part keep fragmentation at a minimum.

This is meant for use in trusted environments or apps where security isn't going to be an issue. I made it as a convenient way to sub allocate in larger memory pools to avoid clogging things up with lots of mallocs everywhere. You can also use it to manage memory ranges on a separate device like GPU memory.

## How do I use it?
Add:
```
#define ZLOC_IMPLEMENTATION
before you include this file in *one* C or C++ file to create the implementation.

// i.e. it should look like this:
#include ...
#include ...
#include ...
#define ZLOC_IMPLEMENTATION
#include "2loc.h"
```

The interface is very straightforward. Simply allocate a block of memory that you want to use for your pool and then call zloc_Allocate to allocate blocks within that pool and zloc_Free when you're done with an allocation. Don't forget to free the orinal memory you created in the first place. The Allocator doesn't care what you use to create the memory to use with the allocator only that it's read and writable to.

## The main commands you will use

```zloc_InitialiseAllocator(void *memory_pointer);```

This initialises an allocator without a pool so it just sets up the necessary allocator data structure to manage all the blocks that are allocated. The memory pointer must point to memory that is of size zloc_AllocatorSize() or sizeof(zloc_allocator).

```zloc_AddPool(zloc_allocator *allocator, void *memory_pointer, zloc_size size_of_memory);```

Takes an allocator that you initiliased already and adds a pool of memory to it which will be used by the allocator to allocate blocks of memory. You can add as many pools to the allocator as you want and they can be of varying sizes.

```zloc_InitialiseAllocatorWithPool(void *memory_pointer, zloc_size size);```

This will set up an allocator and add a memory pool to it at the same time for convenience. The allocator structure will sit at the start of the memory you pass to the function. The size you pass must be the same size as the memory you pass. The remaining space after the allocator struct in the memory will be used as the pool.

```zloc_Allocate(zloc_allocator *allocator, zloc_size size);```

Allocate a block of memory using the allocator you pass to the function. The allocator will search the free blocks and split the nearest sized block requested and return a pointer to the allocation or 0 if the allocator found no suitable free blocks.

```zloc_Free(zloc_allocator *allocator, void *pointer);```

Free a previously allocated memory block. Just pass the allocator that allocated the memory and a pointer to the actual allocated memory. The memory block will be merged with neighbouring free blocks and then added back into the allocator's list of free blocks.

```zloc_RemovePool(zloc_allocator *allocator, zloc_pool *pool);```

Remove a pool from the allocator that you previously added. You can only do this if all blocks in the pool are free.

## Here's some basic usage examples:

```
zloc_size size = 1024 * 1024 * 128;	//128MB
void *memory = malloc(size);
zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
assert(allocator); Something went wrong, unable to initialise the allocator

int *int_allocation = zloc_Allocate(allocator, sizeof(int) * 100);
if(int_allocation) {
	for (int i = 0; i != 100; ++i) {
		int_allocation[i] = rand();
	}
	for (int i = 0; i != 100; ++i) {
		printf("%i\n", int_allocation[i]);
	}
	assert(zloc_Free(allocator, int_allocation));	//Unable to free the allocation
} else {
	//Unable to allocate
}
free(memory);
```

```
//Add a pool separately
zloc_allocator *allocator = (zloc_allocator*)malloc(zloc_AllocatorSize());
allocator = zloc_InitialiseAllocator(allocator);
assert(allocator); Something went wrong, unable to initialise the allocator
zloc_size size = 1024 * 1024 * 128;	//128MB
void *memory = malloc(size);
zloc_pool *pool = zloc_AddPool(memory, size);

int *int_allocation = zloc_Allocate(allocator, sizeof(int) * 100);
if(int_allocation) {
	for (int i = 0; i != 100; ++i) {
		int_allocation[i] = rand();
	}
	for (int i = 0; i != 100; ++i) {
		printf("%i\n", int_allocation[i]);
	}
	assert(zloc_Free(allocator, int_allocation));	//Unable to free the allocation
	zloc_RemovePool(allocator, pool);
} else {
	//Unable to allocate
}
free(memory);
free((void*)allocator);
```

You can also take a look at the tests.c file for more examples of usage. If you want to run threaded tests on windows then you'll need to grab pthred for windows and add pthread.h and the dll or static lib to your compile.

Is it thread safe?
define *ZLOC_THREAD_SAFE* before you include 2loc.h to make each call to zloc_Allocate and zloc_Free lock down the allocator. Basically all it does is lock the allocator so that only one process can free or allocate at the same time. Future versions would probably handle this with separate pools per thread.

Other options:
Define *ZLOC_OUTPUT_ERROR_MESSAGES* to switch on logging errors to the console for some more feedback on errors like out of memory or corrupted block detection.

Define *ZLOC_MAX_SIZE_INDEX* to alter the maximum block size the allocator can handle. Default in 64bit is ~34GB. The size is determined by 1 << ZLOC_MAX_SIZE_INDEX or 2 ^ ZLOC_MAX_SIZE_INDEX. Any value below 64 is acceptable. You can reduce the number to save some space in the allocator structure but it really won't save much.

Define *ZLOC_OUTPUT_ERROR_MESSAGES* to have any errors output to the console. This includes warnings like out of memory in the allocator (not system memory).

Define *ZLOC_ENABLE_REMOTE_MEMORY* to enable features that let you extend the block header to manage memory on a separate device such as a gpu.
