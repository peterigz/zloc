// TLSFAllocator.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <math.h>
#include "header.h"

#define tfxKilobyte(Value) ((Value)*1024LL)
#define tfxMegabyte(Value) (tfxKilobyte(Value)*1024LL)
#define tfxGigabyte(Value) (tfxMegabyte(Value)*1024LL)
#define tfxMin(a, b) (((a) < (b)) ? (a) : (b))
#define tfxMax(a, b) (((a) > (b)) ? (a) : (b))

typedef unsigned int TLocIndex;
typedef unsigned int u32;

#ifndef SECOND_LEVEL_SUBDIVISIONS_LOG2
#define SECOND_LEVEL_SUBDIVISIONS_LOG2 5
#endif

#ifndef MEMORY_ALIGNMENT_LOG2
#define MEMORY_ALIGNMENT_LOG2 3
#endif

#define MIN_BLOCK_SIZE 16
const u32 second_level_indexes = 1 << SECOND_LEVEL_SUBDIVISIONS_LOG2;

enum tloc_constants {
	MEMORY_ALIGNMENT = 1 << MEMORY_ALIGNMENT_LOG2,
	MINIMUM_BLOCK_SIZE = 16,
	SECOND_LEVEL_INDEX_COUNT = 1 << SECOND_LEVEL_SUBDIVISIONS_LOG2,
	FIRST_LEVEL_INDEX_MAX = 32,
	FIRST_LEVEL_INDEX_SHIFT = SECOND_LEVEL_SUBDIVISIONS_LOG2 + MEMORY_ALIGNMENT_LOG2,
	FIRST_LEVEL_INDEX_COUNT = FIRST_LEVEL_INDEX_MAX - FIRST_LEVEL_INDEX_SHIFT + 1,
	BLOCK_IS_FREE = 1 << 0,
	PREV_BLOCK_IS_FREE = 1 << 1
};

typedef struct TLoc {
	u32 first_level_index_count;
	u32 first_level_bitmap;
	u32 second_level_indexes[SECOND_LEVEL_INDEX_COUNT];
	block_header_t* blocks[FL_INDEX_COUNT][SECOND_LEVEL_INDEX_COUNT];
} TLoc;

TLoc *InitialiseMemoryPool(void *memory, u32 size) {
	TLoc *parameters = (TLoc*)memory;
	TLocIndex fli_index;
	memset(parameters, 0, sizeof(TLoc));
	_BitScanReverse(&fli_index, size);
	parameters->first_level_index_count = tfxMin(fli_index, 31);
}

typedef struct TLocHeader {
	struct TLocHeader *prev_physical_block;
	size_t size;
	struct TLocHeader *prev_free_block;
	struct TLocHeader *next_free_block;
} TLocHeader;

inline TLocIndex calculate_first_level_index(u32 size) {
	TLocIndex index;
	_BitScanReverse(&index, size);
	return index;
}

inline TLocIndex calculate_second_level_index(u32 size, TLocIndex first_level_index) {
	size = size & ~(1 << first_level_index);
	size = size >> (first_level_index - SECOND_LEVEL_SUBDIVISIONS_LOG2);
	return size;
}

int main()
{

	TLocIndex fi = calculate_first_level_index(460);
	TLocIndex si = calculate_second_level_index(460, fi);
		
    printf("F: %u, S: %u\n", fi, si);
	
}
