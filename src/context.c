#include "pbc.h"
#include "alloc.h"
#include "varint.h"
#include "context.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _MSC_VER
#include <stdbool.h>
#endif

#define INNER_ATOM ((PBC_CONTEXT_CAP - sizeof(struct context)) / sizeof(struct atom))

/* 不再使用任何线程局部缓存：pbc_ctx 是调用方栈上的一块裸内存，可能在不同线程
 * 之间传递，thread_local 池既没有可靠的生命周期，也会被 DLL 的 __declspec(thread)
 * 限制。_pbcC_open 现在只在 rmessage_new / pattern_unpack 这类低频加载路径使用，
 * 单次解码走 pbc_decode 的单趟解析，内联槽不够时直接 malloc，close 时 free。 */

int
_pbcC_next(void *buffer, int cap, struct atom *a, int start)
{
	uint8_t * p = (uint8_t *)buffer;
	struct longlong r;
	/* tag、长度、varint 都按实际剩余字节解码，不再拷贝到 10 字节临时缓冲 */
	int len = _pbcV_decode(p, cap, &r);
	if (len == 0 || r.hi != 0)
		return 0;

	int wiretype = r.low & 7;
	a->wire_id = r.low;
	p += len;
	start += len;
	cap -=len;

	switch (wiretype) {
	case WT_VARINT :
		len = _pbcV_decode(p, cap, &a->v.i);
		if (len == 0)
			return 0;
		return len + (int)(p - (uint8_t *)buffer);
	case WT_BIT64 :
		if (cap < 8)
			return 0;
		a->v.i.low = p[0] |
			p[1] << 8 |
			p[2] << 16 |
			p[3] << 24;
		a->v.i.hi = p[4] |
			p[5] << 8 |
			p[6] << 16 |
			p[7] << 24;
		return len + 8;
	case WT_LEND :
		len = _pbcV_decode(p, cap, &r);
		if (len == 0 || r.hi != 0 || (uint64_t)cap < (uint64_t)len + r.low)
			return 0;
		a->v.s.start = start + len;
		a->v.s.end = start + len + r.low;
		return len + r.low + (int)(p - (uint8_t *)buffer);
	case WT_BIT32 :
		if (cap < 4)
			return 0;
		a->v.i.low = p[0] |
			p[1] << 8 |
			p[2] << 16 |
			p[3] << 24;
		a->v.i.hi = 0;
		return len + 4;
	default:
		return 0;
	}
}

static inline int
_decode_varint(uint8_t * buffer, int size , struct atom * a) {
	a->wire_id = WT_VARINT;
	return _pbcV_decode(buffer, size, &(a->v.i));
}

static int
_open_packed_varint(struct context * ctx , uint8_t * buffer, int size) {
	/* 每个 varint 至少占 1 字节，元素个数不超过 size，一次按上界分配即可 */
	int cap = size > INNER_ATOM ? size : INNER_ATOM;
	struct atom * a = (struct atom *)(ctx + 1);
	if (cap > INNER_ATOM) {
		a = (struct atom *)malloc((size_t)cap * sizeof(struct atom));
		if (a == NULL) {
			ctx->a = NULL;
			ctx->number = 0;
			return 0;
		}
	}

	int i = 0;
	while (size > 0 && i < cap) {
		int len = _decode_varint(buffer, size, &a[i]);
		if (len <= 0 || len > size) {
			break;
		}
		buffer += len;
		size -= len;
		++i;
	}

	ctx->a = a;
	ctx->number = i;

	return i;
}

int
_pbcC_open_packed(pbc_ctx _ctx, int ptype, void *buffer, int size) {
	struct context * ctx = (struct context *)_ctx;
	ctx->buffer = (char *)buffer;
	ctx->size = size;
	ctx->number = 0;
	ctx->a = NULL;

	if (buffer == NULL || size == 0) {
		return 0;
	}

	int bits = 0;

	switch (ptype) {
	case PTYPE_INT64:
	case PTYPE_UINT64:
	case PTYPE_INT32:
	case PTYPE_BOOL:
	case PTYPE_UINT32:
	case PTYPE_ENUM:
	case PTYPE_SINT32:
	case PTYPE_SINT64:
		return _open_packed_varint(ctx , (uint8_t *)buffer, size);
	case PTYPE_DOUBLE:
	case PTYPE_FIXED64:
	case PTYPE_SFIXED64:
		ctx->number = size / 8;
		bits = 64;
		break;
	case PTYPE_FLOAT:
	case PTYPE_FIXED32:
	case PTYPE_SFIXED32:
		ctx->number = size / 4;
		bits = 32;
		break;
	default:
		return 0;
	}

	struct atom * a = (struct atom *)(ctx + 1);
	if (ctx->number > INNER_ATOM) {
		a = (struct atom *)malloc((size_t)ctx->number * sizeof(struct atom));
		if (a == NULL) {
			ctx->number = 0;
			return 0;
		}
	}
	ctx->a = a;

	int i;
	if (bits == 64) {
		uint8_t * data = (uint8_t *)buffer;
		for (i=0;i<ctx->number;i++) {
			a[i].wire_id = WT_BIT64;
			a[i].v.i.low = data[0] |
				data[1] << 8 |
				data[2] << 16 |
				data[3] << 24;
			a[i].v.i.hi = data[4] |
				data[5] << 8 |
				data[6] << 16 |
				data[7] << 24;
			data += 8;
		}
	} else {
		uint8_t * data = (uint8_t *)buffer;
		for (i=0;i<ctx->number;i++) {
			a[i].wire_id = WT_BIT32;
			a[i].v.i.low = data[0] |
				data[1] << 8 |
				data[2] << 16 |
				data[3] << 24;
			a[i].v.i.hi = 0;
			data += 4;
		}
	}

	return ctx->number;
}

int
_pbcC_open(pbc_ctx _ctx , void *buffer, int size) {
	struct context * ctx = (struct context *)_ctx;
	ctx->buffer = (char *)buffer;
	ctx->size = size;

	if (buffer == NULL || size == 0) {
		ctx->number = 0;
		ctx->a = NULL;
		return 0;
	}

	struct atom * inline_a = (struct atom *)(ctx + 1);
	struct atom * a = inline_a;
	int cap = INNER_ATOM;
	int i = 0;
	int start = 0;

	ctx->a = a;

	while (size > 0) {
		if (i >= cap) {
			/* 内联槽位用尽。每个字段至少占 2 字节（tag + 1 字节负载），
			 * 所以剩下的字段数不会超过 size/2 + 1，一次 malloc 就够。 */
			int need = i + size / 2 + 1;
			if (need < cap * 2) {
				need = cap * 2;
			}
			struct atom * nb;
			if (a == inline_a) {
				nb = (struct atom *)malloc((size_t)need * sizeof(struct atom));
				if (nb != NULL) {
					memcpy(nb, a, sizeof(struct atom) * i);
				}
			} else {
				nb = (struct atom *)realloc(a, (size_t)need * sizeof(struct atom));
			}
			if (nb == NULL) {
				ctx->number = i;
				return -i;
			}
			a = nb;
			cap = need;
			ctx->a = a;
		}
		int used = _pbcC_next(buffer, size, &a[i], start);
		if (used == 0) {
			ctx->number = i;
			return -i;
		}
		buffer = (char *)buffer + used;
		size -= used;
		start += used;
		++i;
	}
	ctx->number = i;

	return i;
}


void
_pbcC_close(pbc_ctx _ctx) {
	struct context * ctx = (struct context *)_ctx;
	struct atom * inline_a = (struct atom *)(ctx + 1);
	if (ctx->a != NULL && ctx->a != inline_a) {
		free(ctx->a);
		ctx->a = NULL;
	}
}
