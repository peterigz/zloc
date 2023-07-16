// TLSFAllocator.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <math.h>
#include "header.h"

#define tfxKilobyte(Value) ((Value)*1024LL)
#define tfxMegabyte(Value) (tfxKilobyte(Value)*1024LL)
#define tfxGigabyte(Value) (tfxMegabyte(Value)*1024LL)
#define tfxMin(a, b) (((a) < (b)) ? (a) : (b))
#define tfxMax(a, b) (((a) > (b)) ? (a) : (b))

typedef unsigned int tlsf_index;
typedef unsigned int u32;
#ifndef SECOND_LEVEL_SUBDIVISIONS
#define SECOND_LEVEL_SUBDIVISIONS 4
#endif
#define MIN_BLOCK_SIZE 16
const u32 second_level_indexes = 1 << SECOND_LEVEL_SUBDIVISIONS;

enum tlsf_constants {
	MEMORY_ALIGNMENT = 8,
	MINIMUM_BLOCK_SIZE = 16,
	SECOND_LEVEL_INDEX_COUNT = 1 << SECOND_LEVEL_SUBDIVISIONS
};

typedef struct tlsf {
	u32 first_level_index_count;
	u32 first_level_bitmap;
	u32 second_level_indexes[SECOND_LEVEL_INDEX_COUNT];
} tlsf;

tlsf *InitialiseMemoryPool(void *memory, u32 size) {
	tlsf *parameters = (tlsf*)memory;
	u32 fli_index;
	memset(parameters, 0, sizeof(tlsf));
	_BitScanReverse(&fli_index, size);
	parameters->first_level_index_count = tfxMin(fli_index, 31);
}

struct header {
	unsigned int size;		//
};

inline u32 calculate_fli(u32 size) {
	u32 fli;
	tfxMin(_BitScanReverse(&fli, size), 31);
	return fli;
}

inline tlsf_index calculate_first_level_index(u32 size) {
	tlsf_index index;
	_BitScanReverse(&index, size);
	return index;
}

inline tlsf_index calculate_second_level_index(u32 size, tlsf_index first_level_index) {
	size = size & ~(1 << first_level_index);
	size = size >> (first_level_index - SECOND_LEVEL_SUBDIVISIONS);
	return size;
}

int main()
{

	size_t s = 4;
	while (s <= tfxGigabyte(1)) {
		printf("Size: %zu = FL: %f\n", s, (float)log2((double)s));
		s = s << 1;
	}

	tlsf_index fi = calculate_first_level_index(300);
	tlsf_index si = calculate_second_level_index(300, fi);
		
    printf("F: %u, S: %u\n", fi, si);
	
}
