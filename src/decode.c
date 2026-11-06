#include "pbc.h"
#include "alloc.h"
#include "context.h"
#include "proto.h"
#include "varint.h"

#include <assert.h>

static const char * TYPENAME[] = {
	"invalid",	// 0
	"integer",	// 1
	"real",	// 2
	"boolean",	// 3
	"enum",	// 4
	"string",	// 5
	"message",	// 6
	"fixed64",	// 7
	"fixed32",	// 8
	"bytes",	// 9
	"int64",	// 10
	"uint",	// 11
};

/* 把字段类型解析成回调参数；seal 阶段的 decode_cache 会预先算好，
 * 这里只在缓存未命中（未知/扩展字段、依赖未解析等）时走一次。 */
static void
resolve_field(struct _field *f, int *type, const char **type_name) {
	*type = _pbcP_type(f, type_name);
	assert(*type != 0);
	if (*type_name == NULL) {
		*type_name = TYPENAME[*type & ~PBC_REPEATED];
	}
}

static int
call_unknown(pbc_decoder f, void * ud, int id, struct atom *a, uint8_t * start) {
	union pbc_value v;
	switch (a->wire_id & 7) {
	case WT_VARINT:
		v.i.low = a->v.i.low;
		v.i.hi = a->v.i.hi;
		f(ud, PBC_INT, TYPENAME[PBC_INT], &v, id , NULL);
		break;
	case WT_BIT64:
		v.i.low = a->v.i.low;
		v.i.hi = a->v.i.hi;
		f(ud, PBC_FIXED64, TYPENAME[PBC_FIXED64], &v, id , NULL);
		break;
	case WT_LEND:
		v.s.buffer = (char*)start + a->v.s.start;
		v.s.len = a->v.s.end - a->v.s.start;
		f(ud, PBC_BYTES, TYPENAME[PBC_BYTES], &v, id , NULL);
		break;
	case WT_BIT32:
		v.i.low = a->v.i.low;
		v.i.hi = 0;
		f(ud, PBC_FIXED32, TYPENAME[PBC_FIXED32], &v, id , NULL);
		break;
	default:
		return 1;
	}
	return 0;
}

static int
call_type(pbc_decoder pd, void * ud, struct _field *f, struct atom *a, uint8_t * start,
		int type, const char *type_name, const char *name) {
	union pbc_value v;
	switch (f->type) {
	case PTYPE_DOUBLE:
		CHECK_BIT64(a, -1);
		v.f = read_double(a);
		break;
	case PTYPE_FLOAT:
		CHECK_BIT32(a, -1);
		v.f = (double) read_float(a);
		break;
	case PTYPE_ENUM:
		CHECK_VARINT(a, -1);
		v.e.id = a->v.i.low;
		v.e.name = (const char *)_pbcM_ip_query(f->type_name.e->id , v.e.id);
		break;
	case PTYPE_INT64:
	case PTYPE_UINT64:
		CHECK_VARINT(a, -1);
		v.i.low = a->v.i.low;
		v.i.hi = a->v.i.hi;
		break;
	case PTYPE_FIXED64:
	case PTYPE_SFIXED64:
		CHECK_BIT64(a, -1);
		v.i.low = a->v.i.low;
		v.i.hi = a->v.i.hi;
		break;
	case PTYPE_INT32:
	case PTYPE_UINT32:
	case PTYPE_BOOL:
		CHECK_VARINT(a, -1);
		v.i.low = a->v.i.low;
		v.i.hi = 0;
		break;
	case PTYPE_FIXED32:
	case PTYPE_SFIXED32:
		CHECK_BIT32(a, -1);
		v.i.low = a->v.i.low;
		v.i.hi = 0;
		break;
	case PTYPE_SINT32:
		CHECK_VARINT(a, -1);
		v.i.low = a->v.i.low;
		v.i.hi = a->v.i.hi;
		_pbcV_dezigzag32((struct longlong *)&(v.i));
		break;
	case PTYPE_SINT64:
		CHECK_VARINT(a, -1);
		v.i.low = a->v.i.low;
		v.i.hi = a->v.i.hi;
		_pbcV_dezigzag64((struct longlong *)&(v.i));
		break;
	case PTYPE_STRING:
	case PTYPE_BYTES:
	case PTYPE_MESSAGE:
		CHECK_LEND(a, -1);
		v.s.buffer = start + a->v.s.start;
		v.s.len = a->v.s.end - a->v.s.start;
		break;
	default:
		assert(0);
		break;
	}
	pd(ud, type, type_name, &v, f->id, name);
	return 0;
}

static int
call_array(pbc_decoder pd, void * ud, struct _field *f, uint8_t * buffer , int size,
		int type, const char *type_name, const char *name) {
	union pbc_value v;
	v.i.hi = 0;
	int i;
	switch(f->type) {
		case PTYPE_DOUBLE:
			if (size % 8 != 0) {
				return -1;
			}
			for (i=0;i<size;i+=8) {
				union {
					double d;
					uint64_t i64;
				} u;
				u.i64 = (uint64_t)buffer[i] |
					(uint64_t)buffer[i+1] << 8 |
					(uint64_t)buffer[i+2] << 16 |
					(uint64_t)buffer[i+3] << 24 |
					(uint64_t)buffer[i+4] << 32 |
					(uint64_t)buffer[i+5] << 40 |
					(uint64_t)buffer[i+6] << 48 |
					(uint64_t)buffer[i+7] << 56;
				v.f = u.d;
				pd(ud, type, type_name, &v, f->id, name);
			}
			return size/8;
		case PTYPE_FLOAT:
			if (size % 4 != 0)
				return -1;
			for (i=0;i<size;i+=4) {
				union {
					float f;
					uint32_t i32;
				} u;
				u.i32 = (uint32_t)buffer[i] |
					(uint32_t)buffer[i+1] << 8 |
					(uint32_t)buffer[i+2] << 16 |
					(uint32_t)buffer[i+3] << 24;
				v.f = (double)u.f;
				pd(ud, type, type_name, &v, f->id, name);
			}
			return size/4;
		case PTYPE_FIXED32:
		case PTYPE_SFIXED32:
			if (size % 4 != 0)
				return -1;
			for (i=0;i<size;i+=4) {
				v.i.low = (uint32_t)buffer[i] |
					(uint32_t)buffer[i+1] << 8 |
					(uint32_t)buffer[i+2] << 16 |
					(uint32_t)buffer[i+3] << 24;
				pd(ud, type, type_name, &v, f->id, name);
			}
			return size/4;
		case PTYPE_FIXED64:
		case PTYPE_SFIXED64:
			if (size % 8 != 0)
				return -1;
			for (i=0;i<size;i+=8) {
				v.i.low = (uint32_t)buffer[i] |
					(uint32_t)buffer[i+1] << 8 |
					(uint32_t)buffer[i+2] << 16 |
					(uint32_t)buffer[i+3] << 24;
				v.i.hi = (uint32_t)buffer[i+4] |
					(uint32_t)buffer[i+5] << 8 |
					(uint32_t)buffer[i+6] << 16 |
					(uint32_t)buffer[i+7] << 24;
				pd(ud, type, type_name, &v, f->id, name);
			}
			return size/8;
		case PTYPE_INT64:
		case PTYPE_UINT64:
		case PTYPE_INT32:
		case PTYPE_UINT32:
		case PTYPE_BOOL: {
			int n = 0;
			while (size > 0) {
				int len = _pbcV_decode(buffer, size, (struct longlong *)&(v.i));
				if (len == 0)
					return -1;
				pd(ud, type, type_name, &v, f->id, name);
				buffer += len;
				size -= len;
				++n;
			}
			return n;
		}
		case PTYPE_ENUM: {
			int n = 0;
			while (size > 0) {
				int len = _pbcV_decode(buffer, size, (struct longlong *)&(v.i));
				if (len == 0)
					return -1;
				v.e.id = v.i.low;
				v.e.name = (const char *)_pbcM_ip_query(f->type_name.e->id , v.i.low);
				pd(ud, type, type_name, &v, f->id, name);
				buffer += len;
				size -= len;
				++n;
			}
			return n;
		}
		case PTYPE_SINT32: {
			int n = 0;
			while (size > 0) {
				int len = _pbcV_decode(buffer, size, (struct longlong *)&(v.i));
				if (len == 0)
					return -1;
				_pbcV_dezigzag32((struct longlong *)&(v.i));
				pd(ud, type, type_name, &v, f->id, name);
				buffer += len;
				size -= len;
				++n;
			}
			return n;
		}
		case PTYPE_SINT64: {
			int n = 0;
			while (size > 0) {
				int len = _pbcV_decode(buffer, size, (struct longlong *)&(v.i));
				if (len == 0)
					return -1;
				_pbcV_dezigzag64((struct longlong *)&(v.i));
				pd(ud, type, type_name, &v, f->id, name);
				buffer += len;
				size -= len;
				++n;
			}
			return n;
		}
		default:
			return -1;
	}
}

/* 紧凑缓存按 field id 升序排列，二分查找。字段数通常 < 32，最多 5 次比较。比hash快 */
static struct pbc_dfield *
_pbcP_find_dfield(struct pbc_dfield *cache, int n, int id) {
	int lo = 0;
	int hi = n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		int v = cache[mid].pub.id;
		if (v == id)
			return &cache[mid];
		if (v < id)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* 派发一个已解析出来的字段（tag + 负载在 a 里）。返回 0 成功，-1 失败。 */
static int
decode_atom(struct _message * msg, struct pbc_dfield * cache, struct atom * a,
		uint8_t * start, pbc_decoder pd, void * ud) {
	int id = a->wire_id >> 3;
	struct _field * f = NULL;
	int type = 0;
	const char * type_name = NULL;
	const char * name = NULL;

	/* 纯读注册阶段预编译的缓存，跳过 _pbcM_ip_query 与 _pbcP_type */
	struct pbc_dfield * dc = _pbcP_find_dfield(cache, msg->decode_cap, id);
	if (dc) {
		f = dc->f;
		type = dc->pub.type;
		name = dc->pub.name;
		type_name = dc->pub.type_name;
		if (type_name == NULL)
			type_name = TYPENAME[type & ~PBC_REPEATED];
	} else {
		f = (struct _field *)_pbcM_ip_query(msg->id , id);
		if (f) {
			resolve_field(f, &type, &type_name);
			name = f->name;
		}
	}

	if (f == NULL) {
		return call_unknown(pd,ud,id,a,start) ? -1 : 0;
	}
	if (f->label == LABEL_PACKED) {
		if ((a->wire_id & 7) == WT_LEND) {
			int n = call_array(pd, ud, f , start + a->v.s.start , a->v.s.end - a->v.s.start,
				type, type_name, name);
			return n < 0 ? -1 : 0;
		}
		/* proto3 规范：声明 packed 的字段也必须能接受 unpacked 编码
		 * （proto2 发送方或显式 [packed=false]） */
	}
	return call_type(pd,ud,f,a,start,type,type_name,name);
}

int
pbc_decode(struct pbc_env * env, const char * type_name , struct pbc_slice * slice, pbc_decoder pd, void *ud) {
	struct _message * msg = _pbcP_get_message(env, type_name);
	if (msg == NULL) {
		env->lasterror = "Proto not found";
		return -1;
	}
	if (slice->len == 0) {
		return 0;
	}
	uint8_t * start = (uint8_t *)slice->buffer;
	/* env 在 _pbcP_seal 之后只读；这里纯读，不写任何共享状态 */
	struct pbc_dfield * cache = msg->decode_cache;

	/* 单趟解码：边解析 wire 边分派。
	 * 原实现要先 _pbcC_open 把整个包预解析成 atom 数组（字段多时还要 malloc/realloc
	 * 一遍），第二趟才遍历数组回调。预解析的全部结果就是这里逐个解出来的 tag+负载，
	 * 所以直接就地分派即可，省掉一趟完整扫描和一整块临时数组。
	 * 子 message 由回调重入 pbc_decode，游标都在栈上，互不影响。 */
	int size = slice->len;
	int offset = 0;
	int number = 0;

	while (size > 0) {
		struct atom a;
		int used = _pbcC_next(start + offset, size, &a, offset);
		if (used == 0) {
			env->lasterror = "decode context error";
			return -number - 1;
		}
		if (decode_atom(msg, cache, &a, start, pd, ud) != 0) {
			return -number - 1;
		}
		offset += used;
		size -= used;
		++number;
	}
	return number;
}

/* ---------------------------------------------------------------------------
 * 扁平快速路径
 *
 * 与旧 pbc_decode 的区别：不构造 struct atom、不转 union pbc_value、不再按
 * 字段类型做第二趟 switch，也不按 schema 字符串重查子 message。这里就地读
 * tag、按 id 二分找元数据、按字段类型直接从 slice 取一次值，然后一次回调
 * 交给 C++ binding 做 lua_rawset。子 message 的指针、字段数都已在 seal 阶段
 * 算好，随 env 只读。
 * --------------------------------------------------------------------------- */

#include <string.h>

static inline uint32_t
_le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
_le64(const uint8_t *p) {
	return (uint64_t)_le32(p) | (uint64_t)_le32(p + 4) << 32;
}

static inline double
_as_double(uint64_t u) {
	union { uint64_t i; double d; } v;
	v.i = u;
	return v.d;
}

static inline float
_as_float(uint32_t u) {
	union { uint32_t i; float f; } v;
	v.i = u;
	return v.f;
}

/* 返回消耗的字节数，0 表示数据非法。不越界读，不需要 10 字节补齐。 */
static inline int
_read_varint(const uint8_t *p, const uint8_t *end, uint64_t *out) {
	const uint8_t *q = p;
	uint64_t v = 0;
	int shift = 0;
	while (q < end && shift < 70) {
		uint8_t b = *q++;
		v |= (uint64_t)(b & 0x7f) << shift;
		if ((b & 0x80) == 0) {
			*out = v;
			return (int)(q - p);
		}
		shift += 7;
	}
	return 0;
}

static inline int32_t
_dezigzag32(uint32_t n) {
	return (int32_t)(n >> 1) ^ -(int32_t)(n & 1);
}

static inline int64_t
_dezigzag64(uint64_t n) {
	return (int64_t)(n >> 1) ^ -(int64_t)(n & 1);
}

/* 单个（非 packed）字段：把 tag 之后已经读出的负载转成紧凑值。
 * enum 一律按整数值交给上层（proto 的 enum 在 wire 上就是 varint），
 * binding 侧不需要再和整数/字符串两种形态打交道。 */
static void
_fill_value(struct _field *f, uint64_t iv, const uint8_t *data, int len,
		struct pbc_fvalue *v) {
	memset(v, 0, sizeof(*v));
	switch (f->type) {
	case PTYPE_DOUBLE:
		v->f = _as_double(iv);
		break;
	case PTYPE_FLOAT:
		v->f = (double)_as_float((uint32_t)iv);
		break;
	case PTYPE_FIXED32:
	case PTYPE_SFIXED32:
		v->i = (uint32_t)iv;
		break;
	case PTYPE_FIXED64:
	case PTYPE_SFIXED64:
		v->i = iv;
		break;
	case PTYPE_SINT32:
		v->i = (uint64_t)(int64_t)_dezigzag32((uint32_t)iv);
		break;
	case PTYPE_SINT64:
		v->i = (uint64_t)_dezigzag64(iv);
		break;
	case PTYPE_STRING:
	case PTYPE_BYTES:
	case PTYPE_MESSAGE:
		v->s = (const char *)data;
		v->len = len;
		break;
	default:
		v->i = iv;
		break;
	}
}

/* packed 数组里的一个元素，从 *q 顺序读并推进游标 */
static int
_fill_packed_one(struct _field *f, const uint8_t **q, const uint8_t *end,
		struct pbc_fvalue *v) {
	memset(v, 0, sizeof(*v));
	switch (f->type) {
	case PTYPE_DOUBLE:
		if (end - *q < 8) return -1;
		v->f = _as_double(_le64(*q));
		*q += 8;
		break;
	case PTYPE_FLOAT:
		if (end - *q < 4) return -1;
		v->f = (double)_as_float(_le32(*q));
		*q += 4;
		break;
	case PTYPE_FIXED64:
	case PTYPE_SFIXED64:
		if (end - *q < 8) return -1;
		v->i = _le64(*q);
		*q += 8;
		break;
	case PTYPE_FIXED32:
	case PTYPE_SFIXED32:
		if (end - *q < 4) return -1;
		v->i = _le32(*q);
		*q += 4;
		break;
	default: {
		uint64_t x;
		int l = _read_varint(*q, end, &x);
		if (l == 0) return -1;
		*q += l;
		switch (f->type) {
		case PTYPE_SINT32:
			v->i = (uint64_t)(int64_t)_dezigzag32((uint32_t)x);
			break;
		case PTYPE_SINT64:
			v->i = (uint64_t)_dezigzag64(x);
			break;
		default:
			v->i = x;
			break;
		}
		break;
	}
	}
	return 0;
}

/* 预算 packed 元素个数，让 binding 一次预分配好 Lua 数组 */
static int
_count_packed(struct _field *f, const uint8_t *p, const uint8_t *end) {
	switch (f->type) {
	case PTYPE_DOUBLE:
	case PTYPE_FIXED64:
	case PTYPE_SFIXED64:
		return (int)((end - p) / 8);
	case PTYPE_FLOAT:
	case PTYPE_FIXED32:
	case PTYPE_SFIXED32:
		return (int)((end - p) / 4);
	default: {
		int n = 0;
		while (p < end) {
			uint8_t b;
			do {
				b = *p++;
			} while ((b & 0x80) && p < end);
			++n;
		}
		return n;
	}
	}
}

int
pbc_decode_fast_msg(struct pbc_msg *m_, struct pbc_slice *slice,
		pbc_fast_decoder cb, void *ud) {
	struct _message *msg = (struct _message *)m_;
	if (msg == NULL)
		return -1;
	if (slice == NULL || slice->len <= 0)
		return 0;

	const uint8_t *p = (const uint8_t *)slice->buffer;
	const uint8_t *end = p + slice->len;
	struct pbc_dfield *cache = msg->decode_cache;
	int n = msg->decode_cap;
	int number = 0;

	while (p < end) {
		uint64_t tag;
		int l = _read_varint(p, end, &tag);
		if (l == 0 || tag > 0xffffffffULL)
			return -1;
		p += l;
		int id = (int)(tag >> 3);
		int wire = (int)(tag & 7);

		const uint8_t *data = NULL;
		int data_len = 0;
		int used = 0;
		uint64_t iv = 0;
		switch (wire) {
		case WT_VARINT:
			used = _read_varint(p, end, &iv);
			if (used == 0) return -1;
			break;
		case WT_BIT64:
			if (end - p < 8) return -1;
			iv = _le64(p);
			used = 8;
			break;
		case WT_BIT32:
			if (end - p < 4) return -1;
			iv = _le32(p);
			used = 4;
			break;
		case WT_LEND: {
			uint64_t sz;
			int h = _read_varint(p, end, &sz);
			if (h == 0 || sz > (uint64_t)(end - p - h))
				return -1;
			data = p + h;
			data_len = (int)sz;
			used = h + data_len;
			break;
		}
		default:
			return -1;
		}
		const uint8_t *next = p + used;

		struct pbc_dfield *dc = _pbcP_find_dfield(cache, n, id);
		/* 未知字段与旧路径的 key==NULL 分支一致：直接跳过，不交回调 */
		if (dc != NULL) {
			struct _field *f = dc->f;
			struct pbc_fvalue v;
			if (f->label == LABEL_PACKED && wire == WT_LEND) {
				int count = _count_packed(f, data, data + data_len);
				const uint8_t *q = data;
				const uint8_t *qe = data + data_len;
				while (q < qe) {
					if (_fill_packed_one(f, &q, qe, &v) != 0)
						return -1;
					v.count = count;
					if (cb(ud, &dc->pub, &v) != 0)
						return -1;
				}
			} else {
				_fill_value(f, iv, data, data_len, &v);
				if (cb(ud, &dc->pub, &v) != 0)
					return -1;
			}
		}

		p = next;
		++number;
	}
	return number;
}
