#include "varint.h"

#include "pbc.h"

#include <stdint.h>

inline int
_pbcV_encode32(uint32_t number, uint8_t buffer[10])
{
	if (number < 0x80) {
		buffer[0] = (uint8_t) number ; 
		return 1;
	}
	buffer[0] = (uint8_t) (number | 0x80 );
	if (number < 0x4000) {
		buffer[1] = (uint8_t) (number >> 7 );
		return 2;
	}
	buffer[1] = (uint8_t) ((number >> 7) | 0x80 );
	if (number < 0x200000) {
		buffer[2] = (uint8_t) (number >> 14);
		return 3;
	}
	buffer[2] = (uint8_t) ((number >> 14) | 0x80 );
	if (number < 0x10000000) {
		buffer[3] = (uint8_t) (number >> 21);
		return 4;
	}
	buffer[3] = (uint8_t) ((number >> 21) | 0x80 );
	buffer[4] = (uint8_t) (number >> 28);
	return 5;
}

int
_pbcV_encode(uint64_t number, uint8_t buffer[10]) 
{
	if ((number & 0xffffffff) == number) {
		return _pbcV_encode32((uint32_t)number , buffer);
	}
	int i = 0;
	do {
		buffer[i] = (uint8_t)(number | 0x80);
		number >>= 7;
		++i;
	} while (number >= 0x80);
	buffer[i] = (uint8_t)number;
	return i+1;
}

int
_pbcV_decode(const uint8_t *buffer, int size, struct longlong *result) {
	result->low = 0;
	result->hi = 0;
	if (buffer == NULL || size <= 0) {
		return 0;
	}

	/* varint 最长 10 字节，超过部分不读也不属于本字段 */
	int n = size < 10 ? size : 10;
	uint64_t v = 0;
	int i;
	for (i = 0; i < n; ++i) {
		uint8_t b = buffer[i];
		v |= (uint64_t)(b & 0x7f) << (7 * i);
		if (!(b & 0x80)) {
			/* 与原实现等价：低 32 位进 low，高 32 位进 hi */
			result->low = (uint32_t)v;
			result->hi = (uint32_t)(v >> 32);
			return i + 1;
		}
	}

	/* 10 字节全是 continuation，或 varint 在本段数据内被截断 */
	return 0;
}

int 
_pbcV_zigzag32(int32_t n, uint8_t buffer[10])
{
	n = (n << 1) ^ (n >> 31);
	return _pbcV_encode32(n,buffer);
}

int 
_pbcV_zigzag(int64_t n, uint8_t buffer[10])
{
	n = (n << 1) ^ (n >> 63);
	return _pbcV_encode(n,buffer);
}

void
_pbcV_dezigzag64(struct longlong *r)
{
	uint32_t low = r->low;
	r->low = ((low >> 1) | ((r->hi & 1) << 31)) ^ - (low & 1);
	r->hi = (r->hi >> 1) ^ - (low & 1);
}

void
_pbcV_dezigzag32(struct longlong *r)
{
	uint32_t low = r->low;
	r->low = (low >> 1) ^ - (low & 1);
	r->hi = -(low >> 31);
}
