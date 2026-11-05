#ifndef PROTOBUF_C_VARINT_H
#define PROTOBUF_C_VARINT_H

#include <stdint.h>

struct longlong {
	uint32_t low;
	uint32_t hi;
};

int _pbcV_encode32(uint32_t number, uint8_t buffer[10]);
int _pbcV_encode(uint64_t number, uint8_t buffer[10]);
int _pbcV_zigzag32(int32_t number, uint8_t buffer[10]);
int _pbcV_zigzag(int64_t number, uint8_t buffer[10]);

/* 带长度的 varint 解码：只读 buffer[0..size-1]，最多 10 字节。
 * 返回消耗的字节数；数据被截断或 10 字节全是 continuation 时返回 0。
 * 调用方不需要把尾部补进 10 字节临时缓冲，也不会越界读。 */
int _pbcV_decode(const uint8_t *buffer, int size, struct longlong *result);
void _pbcV_dezigzag64(struct longlong *r);
void _pbcV_dezigzag32(struct longlong *r);

#endif
