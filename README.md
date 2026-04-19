# Zest Allocator - A Two Level Segregated Fit Memory Allocator

This software is dual-licensed to the public domain and under the following license: you are granted a perpetual, irrevocable license to copy, modify, publish, and distribute this file as you see fit. See LICENSE file for details.

This library is a single header mimimal allocator based on the following paper: http://www.gii.upv.es/tlsf/files/ecrts04_tlsf.pdf

Thanks to the authors of the paper and also Sean Barret for his how to make a single header-file library guidelines, and also to Matthew Conte who's own TLSF lib I referenced when trying to understand how the algorythm works. His library can be found here: https://github.com/mattconte/tlsf

## What's this library for?
This library is for sub allocating memory blocks within larger memory allocation pools that you might create with malloc or VirtualAlloc etc.

Allocating and freeing those memory blocks happens at O(1) time complexity and should for the most part keep fragmentation at a minimum.

This is meant for use in trusted environments or apps where security isn't going to be an issue. I made it as a convenient way to sub allocate in larger memory pools to avoid clogging things up with lots of mallocs everywhere. You can also use it to manage memory ranges on a separate device like GPU memory.

A small linear (arena) allocator is bundled alongside the main allocator for transient scratch memory.

## How do I use it?
Add `#define ZLOC_IMPLEMENTATION` before you include this file in *one* C or C++ file to create the implementation.

```c
// i.e. it should look like this:
#include ...
#include ...
#include ...
#define ZLOC_IMPLEMENTATION
#include "zloc.h"
```

The interface is very straightforward. Simply allocate a block of memory that you want to use for your pool and then call zloc_Allocate to allocate blocks within that pool and zloc_Free when you're done with an allocation. Don't forget to free the orinal memory you created in the first place. The Allocator doesn't care what you use to create the memory to use with the allocator only that it's read and writable to.

## The main commands you will use

```c
zloc_InitialiseAllocator(void *memory_pointer);
```

This initialises an allocator without a pool so it just sets up the necessary allocator data structure to manage all the blocks that are allocated. The memory pointer must point to memory that is of size zloc_AllocatorSize() or sizeof(zloc_allocator).

```c
zloc_AddPool(zloc_allocator *allocator, void *memory_pointer, zloc_size size_of_memory);
```

Takes an allocator that you initiliased already and adds a pool of memory to it which will be used by the allocator to allocate blocks of memory. You can add as many pools to the allocator as you want and they can be of varying sizes.

```c
zloc_InitialiseAllocatorWithPool(void *memory_pointer, zloc_size size);
```

This will set up an allocator and add a memory pool to it at the same time for convenience. The allocator structure will sit at the start of the memory you pass to the function. The size you pass must be the same size as the memory you pass. The remaining space after the allocator struct in the memory will be used as the pool.

```c
zloc_Allocate(zloc_allocator *allocator, zloc_size size);
```

Allocate a block of memory using the allocator you pass to the function. The allocator will search the free blocks and split the nearest sized block requested and return a pointer to the allocation or 0 if the allocator found no suitable free blocks.

```c
zloc_AllocateAligned(zloc_allocator *allocator, zloc_size size, zloc_size alignment);
```

Allocate a block of memory with a specific alignment. The alignment must be a power of two. Use this when the consumer of the allocation needs a stricter alignment than the default - SIMD types, page-aligned buffers, GPU staging buffers and the like.

```c
zloc_Reallocate(zloc_allocator *allocator, void *ptr, zloc_size size);
```

Resize an existing allocation. If the next physical block is free and large enough the block will be grown in place; otherwise a new block is found, the old contents are copied across, and the original is freed. Passing `size = 0` frees the allocation and returns NULL. Passing `ptr = NULL` is equivalent to `zloc_Allocate`.

```c
zloc_Free(zloc_allocator *allocator, void *pointer);
```

Free a previously allocated memory block. Just pass the allocator that allocated the memory and a pointer to the actual allocated memory. The memory block will be merged with neighbouring free blocks and then added back into the allocator's list of free blocks.

```c
zloc_RemovePool(zloc_allocator *allocator, zloc_pool *pool);
```

Remove a pool from the allocator that you previously added. You can only do this if all blocks in the pool are free.

## Here's some basic usage examples:

```c
zloc_size size = 1024 * 1024 * 128;	//128MB
void *memory = malloc(size);
zloc_allocator *allocator = zloc_InitialiseAllocatorWithPool(memory, size);
assert(allocator);  //Something went wrong, unable to initialise the allocator

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

```c
//Add a pool separately
zloc_allocator *allocator = (zloc_allocator*)malloc(zloc_AllocatorSize());
allocator = zloc_InitialiseAllocator(allocator);
assert(allocator);  //Something went wrong, unable to initialise the allocator
zloc_size size = 1024 * 1024 * 128;	//128MB
void *memory = malloc(size);
zloc_pool *pool = zloc_AddPool(allocator, memory, size);

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

You can also take a look at the tests.c file for more examples of usage. If you want to run threaded tests on windows then you'll need to grab pthread for windows and add pthread.h and the dll or static lib to your compile.

## Linear (arena) allocator

Bundled alongside the main allocator is a small linear allocator for cases where you don't need individual frees, just a chunk of scratch memory you'll throw away wholesale. Allocations bump a pointer; "freeing" is done by resetting the offset back to zero (or to a previously saved marker), so it's all O(1) with effectively no bookkeeping overhead.

It's handy for per-frame scratch in games, parsing buffers, command-list assembly, and anywhere you'd otherwise reach for alloca() but want a controlled budget.

```c
void *scratch = malloc(1024 * 1024);
zloc_linear_allocator_t arena;
zloc_InitialiseLinearAllocator(&arena, scratch, 1024 * 1024);

void *a = zloc_LinearAllocation(&arena, 4096);
void *b = zloc_LinearAllocation(&arena, 256);

//Reset everything ready for the next frame
zloc_ResetLinearAllocator(&arena);

//Or use markers for nested scopes:
zloc_size mark = zloc_GetMarker(&arena);
void *temp = zloc_LinearAllocation(&arena, 1024);
//...use temp...
zloc_ResetToMarker(&arena, mark);  //rolls back just temp

free(scratch);
```

If a single buffer isn't big enough you can chain allocators together with `zloc_AddNextLinearAllocator`. When the head fills up, allocations spill into the next allocator in the chain automatically. `zloc_GetLinearAllocatorCapacity` reports the total across the chain.

```c
zloc_linear_allocator_t a, b;
zloc_InitialiseLinearAllocator(&a, malloc(64 * 1024), 64 * 1024);
zloc_InitialiseLinearAllocator(&b, malloc(64 * 1024), 64 * 1024);
zloc_AddNextLinearAllocator(&a, &b);
//Allocations through `a` now spill into `b` once `a` runs out
```

There's also a related helper for the main allocator: `zloc_PromoteLinearBlock`. The main use is caching linear allocations so they can be retrieved later. The pattern is: allocate a worst-case-sized block via `zloc_Allocate`, fill it up linearly, and if you decide the result is worth keeping, call `zloc_PromoteLinearBlock` to trim the block down to the bytes you actually used. The unused tail goes back to the free list and the kept block stays at the same address - so any internal pointer references you wrote into the buffer remain valid.

A concrete example is frame graphs: build the graph using a linear allocator, and if it turns out to be one you want to cache, promote it to persistent memory in place rather than rebuilding or copying it.

## Remote memory (managing memory on a GPU or other device)

The allocator has a "remote" mode where the bytes you're tracking aren't the bytes you're walking through to manage them. The classic use case is GPU memory: the data lives in a buffer on the device, but you want to do all the bookkeeping (which sub-ranges are in use, how to split and merge them) on the CPU side where it's cheap and you don't have to round-trip the device.

In remote mode the allocator manages two parallel resources:
- a CPU-side **block memory** buffer that holds the per-block headers (small, but proportional to the number of blocks)
- the actual **remote pool** living on the device (the thing you're really sub-allocating)

The CPU-side blocks are essentially a **proxy** for the device allocations. Each one has the same size and ordering as its counterpart on the device, so when the allocator splits, merges, or reassigns a CPU-side block, it's really describing a change you need to mirror on the device. Crucially the allocator never touches the device memory itself - everything it does is on the CPU-side proxies, and you bridge across to the device through callbacks.

The block headers are extended with a struct of your choice via `zloc_SetBlockExtensionSize`. The first two fields of that struct **must** be `zloc_size size` and `zloc_size memory_offset` (matching `zloc_remote_header`); after those two you're free to add whatever bookkeeping you need - device handles, generation counters, debug info etc.

### Callbacks

Because the allocator can't see your device, you have to tell it what to do at the moments when its view of the world changes. These hooks live as function pointers on `zloc_allocator` (see the `*_callback` fields on the struct):

- **`add_pool_callback`** - fires when you call `zloc_AddRemotePool`, giving you the first block extension so you can initialise things like `memory_offset = 0` and the device handle for the new pool.
- **`split_block_callback`** - fires when an allocation splits a free block in two. The original block is shrunk to the requested size; you get a pointer to the new trimmed block so you can set its `memory_offset` and any device-side state.
- **`merge_next_callback`** / **`merge_prev_callback`** - fire when two adjacent free blocks are coalesced on `zloc_FreeRemote`. The defaults installed by `zloc_InitialiseAllocatorForRemote` already maintain the standard `size`/`memory_offset` fields; override them if your extended header carries state that also needs merging.
- **`unable_to_reallocate_callback`** - fires from `zloc_ReallocateRemote` when the block can't be grown in place and a new one had to be allocated. This is your chance to copy the device-side bytes from old to new before the old gets freed.
- **`get_block_size_callback`** - returns the remote (device-side) size of a block. `zloc_InitialiseAllocatorForRemote` wires this up to read your header's `size` field, so unless you're doing something exotic you can leave it alone.
- **`remote_user_data`** - opaque pointer passed to every callback. Use it to point at whatever per-allocator context you need (the device handle, a queue, an upload ring buffer, etc).

`zloc_AddRemotePool` will assert if the mandatory callbacks (`add_pool_callback`, `split_block_callback`, plus the size/merge ones) aren't set, so you'll find out quickly if you missed one.

### Putting it together

Define `ZLOC_ENABLE_REMOTE_MEMORY` before including the header to compile in this functionality.

```c
//Your extended remote block header. The first two fields are mandatory.
typedef struct gpu_block_header {
	zloc_size size;
	zloc_size memory_offset;
	GpuBufferHandle handle;     //your stuff
} gpu_block_header;

//Callbacks that bridge allocator events across to the device
void on_add_pool(void *user_data, void *block_extension) {
	gpu_block_header *header = (gpu_block_header *)block_extension;
	header->memory_offset = 0;
	header->handle = ((my_gpu_context *)user_data)->current_pool;
}

void on_split_block(void *user_data, zloc_header *block, zloc_header *trimmed, zloc_size remote_size) {
	gpu_block_header *original = (gpu_block_header *)zloc_BlockUserExtensionPtr(block);
	gpu_block_header *new_tail = (gpu_block_header *)zloc_BlockUserExtensionPtr(trimmed);
	new_tail->memory_offset = original->memory_offset + remote_size;
	new_tail->handle = original->handle;
}

void on_reallocation_copy(void *user_data, zloc_header *old_block, zloc_header *new_block) {
	gpu_block_header *old_h = (gpu_block_header *)zloc_BlockUserExtensionPtr(old_block);
	gpu_block_header *new_h = (gpu_block_header *)zloc_BlockUserExtensionPtr(new_block);
	gpu_copy(old_h->handle, old_h->memory_offset, new_h->handle, new_h->memory_offset, old_h->size);
}

//Set up the CPU-side allocator that will track GPU memory
void *allocator_memory = malloc(zloc_AllocatorSize());
zloc_allocator *allocator = zloc_InitialiseAllocatorForRemote(allocator_memory);

//Smallest allocation granularity on the device, plus the size of your extended header
zloc_SetMinimumAllocationSize(allocator, 256);
zloc_SetBlockExtensionSize(allocator, sizeof(gpu_block_header));

//Wire up the bridge to the device. The defaults from InitialiseAllocatorForRemote cover
//get_block_size / merge_next / merge_prev; you fill in the rest.
allocator->remote_user_data = &my_context;
allocator->add_pool_callback = on_add_pool;
allocator->split_block_callback = on_split_block;
allocator->unable_to_reallocate_callback = on_reallocation_copy;

//Allocate the GPU pool with whatever your graphics API gives you
zloc_size gpu_pool_size = 64 * 1024 * 1024;
my_context.current_pool = gpu_alloc(gpu_pool_size);

//Work out how much CPU memory we need to bookkeep that GPU pool, then add it to the allocator.
//on_add_pool will fire from inside this call to initialise the first block's offset/handle.
zloc_size cpu_tracking_size = zloc_CalculateRemoteBlockPoolSize(allocator, gpu_pool_size);
void *cpu_tracking = malloc(cpu_tracking_size);
zloc_AddRemotePool(allocator, cpu_tracking, cpu_tracking_size, gpu_pool_size);

//Allocate a 4KB sub-range from the GPU pool. The returned pointer points at YOUR header struct.
//on_split_block fires under the hood to fill in the trimmed remainder.
gpu_block_header *block = (gpu_block_header *)zloc_AllocateRemote(allocator, 4096);
if (block) {
	//block->memory_offset is the byte offset into the GPU pool where the allocation lives
	//block->size is the size of the allocation
	//block->handle was set by on_add_pool / on_split_block
	gpu_upload(block->handle, block->memory_offset, my_data, 4096);
}

//Free it when done. merge_next / merge_prev fire if there are adjacent free blocks to coalesce.
zloc_FreeRemote(allocator, block);
```

You can resize remote allocations with `zloc_ReallocateRemote`, and the same multi-pool model applies - call `zloc_AddRemotePool` again with another GPU buffer if you run out of space.

### Tip: split small and large allocations across separate allocators

The CPU-side bookkeeping cost scales with `remote_pool_size / minimum_allocation_size` - one proxy block per minimum-sized slot. If you set a small minimum (say 256 bytes) so that small allocations don't waste device memory, but the device pool is large (say 1 GB), the proxy buffer ends up enormous even though most real allocations are much bigger.

What I like to do is keep two allocators side by side: one with a small minimum allocation size for small allocations, and one with a much larger minimum (a few KB or more) for big allocations. Route incoming requests to whichever allocator suits the size. The big-allocation allocator can manage a huge device pool with a tiny CPU footprint because each proxy block now stands for a much bigger slot, and the small-allocation allocator stays cheap because its pool is modest. You get fine granularity where you need it without paying for it everywhere.

## Debugging

```c
zloc_VerifyPool(zloc_allocator *allocator, const zloc_pool *pool);
```

Walks the physical block chain of a pool and asserts on any structural corruption it finds (broken back-links, mismatched boundary tags, unmerged adjacent free blocks, misaligned sizes, sentinel damage). Useful in test fixtures or as a "stop the world and check" call when you suspect a buffer overrun has trampled an allocator block.

```c
zloc_CreateMemorySnapshot(const zloc_pool *pool);
```

Walks the same chain and returns a struct of stats: number of free/used blocks and total free/used bytes and for finding memory leaks. Handy for diagnostics or fragmentation reporting.

If you're hunting list corruption, defining `ZLOC_EXTRA_DEBUGGING` makes every push, pop, and remove of a free block run an integrity check on the segregated free lists. It's slow but catches problems within one allocation of when they happen.

## Is it thread safe?
Define *ZLOC_THREAD_SAFE* before you include zloc.h to make each call to zloc_Allocate and zloc_Free lock down the allocator. Basically all it does is lock the allocator so that only one process can free or allocate at the same time. Future versions would probably handle this with separate pools per thread.

## Build options:

Define *ZLOC_OUTPUT_ERROR_MESSAGES* to switch on logging errors to the console for some more feedback on errors like out of memory or corrupted block detection.

Define *ZLOC_THREAD_SAFE* to wrap allocate / free / reallocate calls in a spin-lock so the allocator is safe to share across threads.

Define *ZLOC_SAFEGUARDS* to embed a back-pointer to the owning allocator in every block. Frees and promotes will then assert if the block doesn't actually belong to the allocator you passed - useful for catching context/device allocator mix-ups. Costs one pointer of overhead per block.

Define *ZLOC_EXTRA_DEBUGGING* to run free-list integrity checks on every push, pop, and remove. Slow but catches list corruption synchronously. Pair with `zloc_VerifyPool` calls in your own debug code to also cover physical-chain corruption.

Define *ZLOC_MAX_SIZE_INDEX* to alter the maximum block size the allocator can handle. The size is determined by 1 << ZLOC_MAX_SIZE_INDEX. Default in 64bit is 32 (4GB max block size). Any value below 64 is acceptable. You can reduce the number to save some space in the allocator structure but it really won't save much.

Define *ZLOC_ENABLE_REMOTE_MEMORY* to enable the remote-pool API for managing memory that lives on a separate device (e.g. GPU). See "Remote memory" above.
