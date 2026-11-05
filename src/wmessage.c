#include "pbc.h"
#include "context.h"
#include "alloc.h"
#include "varint.h"
#include "map.h"
#include "proto.h"

#include <stdint.h>
#include <string.h>
#include <assert.h>

#ifndef _MSC_VER
#include <stdbool.h>
#endif

#define WMESSAGE_SIZE 64

/* 编码只有一个 buffer：所有子 message 共享 root 的 buffer，遇到子 message
 * 先写 tag + 长度占位，子层内容直接追加到同一块内存，结束时回写长度。
 *
 * 原来的实现是「每个子 message 一个 pbc_wmessage」：每个都要分配 struct、
 * 64B buffer、pbc_array、512B packed_by_id 并 memset，最后 pbc_wmessage_buffer
 * 再递归 memcpy 拼回父 buffer。BagUpdate 有 20 个 Item，光这些固定开销就占掉
 * 编码耗时的一大半。现在子层只是一个 heap bump 出来的轻量视图（无独立 buffer、
 * 无 memset、无拷贝），与 lua-protobuf 的 pb_addlength 思路一致。
 *
 * packed 字段同样直接把 payload 追加进 buffer：begin 写 [tag][长度占位]，
 * 每元素只编码一次值，end 回写长度。原来要先攒进 pbc_array / map_sp，再在
 * pbc_wmessage_buffer 里第二趟 _pack_packed_* 重编码，那套结构整体删除。 */
struct pbc_wmessage {
	struct _message *type;
	struct pbc_wmessage *root;		/* NULL 表示自己就是 root */
	struct heap *heap;				/* 由 root 拥有 */
	uint8_t *buffer;				/* 仅 root 有效 */
	uint8_t *ptr;
	uint8_t *endptr;
	int len_off;					/* 本视图长度占位在 root buffer 中的偏移，root 为 -1 */
	int packed_len_off;				/* 当前打开的 packed 长度占位偏移，-1 表示没有 */
	struct _field *packed_f;		/* 当前 packed 字段 */
	struct pbc_wmessage *open_sub;	/* 本层最近打开、尚未 end 的子视图 */
	int closed;						/* 视图已回写长度，防止重复 end */
};

static inline struct pbc_wmessage *
_wm_root(struct pbc_wmessage *m) {
	return m->root ? m->root : m;
}

static struct pbc_wmessage *
_wmessage_new(struct heap *h, struct _message *msg) {
	struct pbc_wmessage * m = (struct pbc_wmessage *)_pbcH_alloc(h, sizeof(*m));
	m->type = msg;
	m->root = NULL;
	m->heap = h;
	m->buffer = (uint8_t *)_pbcH_alloc(h, WMESSAGE_SIZE);
	m->ptr = m->buffer;
	m->endptr = m->buffer + WMESSAGE_SIZE;
	m->len_off = -1;
	m->packed_len_off = -1;
	m->packed_f = NULL;
	m->open_sub = NULL;
	m->closed = 0;
	return m;
}

struct pbc_wmessage *
pbc_wmessage_new(struct pbc_env * env, const char *type_name) {
	struct _message * msg = _pbcP_get_message(env, type_name);
	if (msg == NULL)
		return NULL;
	struct heap *h = _pbcH_new(0);
	return _wmessage_new(h, msg);
}

void
pbc_wmessage_delete(struct pbc_wmessage *m) {
	if (m) {
		/* 调用方可能拿的是子视图，heap 归 root 所有 */
		struct pbc_wmessage *r = _wm_root(m);
		_pbcH_delete(r->heap);
	}
}

struct pbc_wmessage *
pbc_wmessage_reset(struct pbc_wmessage *m, const char *type_name) {
	if (m == NULL || type_name == NULL) {
		return NULL;
	}
	/* wmessage 归属于创建它的 env；热更时 binding 会主动 delete 缓存的 wmessage，
	 * 所以这里的 env 一定是当前 env。 */
	struct pbc_wmessage *r = _wm_root(m);
	struct _message * msg = _pbcP_get_message(r->type->env, type_name);
	if (msg == NULL) {
		return NULL;
	}
	struct heap * h = r->heap;
	_pbcH_reset(h);
	return _wmessage_new(h, msg);
}

/* 扩容会让 buffer 重新分配，所有长度占位都记整数偏移而不是指针，因此重分配
 * 之后仍然有效。调用方传 root 或子视图都可以。 */
static void
_expand_message(struct pbc_wmessage *m, int sz) {
	m = _wm_root(m);
	if (m->ptr + sz > m->endptr) {
		int cap = m->endptr - m->buffer;
		sz = m->ptr + sz - m->buffer;
		do {
			cap = cap * 2;
		} while ( sz > cap ) ;
		int old_size = m->ptr - m->buffer;
		uint8_t * buffer = (uint8_t *)_pbcH_alloc(m->heap, cap);
		memcpy(buffer, m->buffer, old_size);
		m->ptr = buffer + (m->ptr - m->buffer);
		m->endptr = buffer + cap;
		m->buffer = buffer;
	}
}

/* 回写一处长度占位：占位时只留 1 字节，长度 >= 128 时扩展 varint 并 memmove。
 * 嵌套是安全的：内层的偏移恒大于外层，memmove 不会动到外层的占位；外层最终
 * 的 payload 长度用回写瞬间的 ptr 计算，包含内层已回写的长度字节。 */
static void
_commit_len(struct pbc_wmessage *m, int off) {
	struct pbc_wmessage *r = _wm_root(m);
	int len = (int)(r->ptr - (r->buffer + off + 1));
	uint8_t temp[10];
	int need = _pbcV_encode32((uint32_t)len, temp);
	if (need > 1) {
		_expand_message(r, need - 1);
		memmove(r->buffer + off + need,
			r->buffer + off + 1, (size_t)len);
		r->ptr += need - 1;
	}
	memcpy(r->buffer + off, temp, need);
}

/* 收掉本层尚未结束的子视图与 packed。每次开始写新字段前调用，所以只持有
 * 「最近一个」子视图就够了：兄弟子消息在下一个 message_f 时自动结束。 */
static void
_flush(struct pbc_wmessage *m) {
	if (m->open_sub) {
		pbc_wmessage_end_sub(m->open_sub);
		m->open_sub = NULL;
	}
	if (m->packed_len_off >= 0) {
		_commit_len(m, m->packed_len_off);
		m->packed_len_off = -1;
		m->packed_f = NULL;
	}
}

/* 只是开始处理一个新字段，不能提交 packed：紧接着的 *_f 可能还是同一个
 * packed 字段的下一个元素，应当继续追加而不是另起一段。 */
static void
_close_open_sub(struct pbc_wmessage *m) {
	if (m->open_sub) {
		pbc_wmessage_end_sub(m->open_sub);
		m->open_sub = NULL;
	}
}

static void
_begin_packed(struct pbc_wmessage *m, struct _field *f) {
	struct pbc_wmessage *r = _wm_root(m);
	int id = f->id << 3 | WT_LEND;
	_expand_message(r, 20);
	r->ptr += _pbcV_encode32(id, r->ptr);
	m->packed_len_off = (int)(r->ptr - r->buffer);
	*r->ptr++ = 0;
	m->packed_f = f;
}

static inline void
int64_encode(uint32_t low, uint32_t hi , uint8_t * buffer) {
	buffer[0] = (uint8_t)(low & 0xff);
	buffer[1] = (uint8_t)(low >> 8 & 0xff);
	buffer[2] = (uint8_t)(low >> 16 & 0xff);
	buffer[3] = (uint8_t)(low >> 24 & 0xff);
	buffer[4] = (uint8_t)(hi & 0xff);
	buffer[5] = (uint8_t)(hi >> 8 & 0xff);
	buffer[6] = (uint8_t)(hi >> 16 & 0xff);
	buffer[7] = (uint8_t)(hi >> 24 & 0xff);
}

static inline void
int32_encode(uint32_t low, uint8_t * buffer) {
	buffer[0] = (uint8_t)(low & 0xff);
	buffer[1] = (uint8_t)(low >> 8 & 0xff);
	buffer[2] = (uint8_t)(low >> 16 & 0xff);
	buffer[3] = (uint8_t)(low >> 24 & 0xff);
}

/* packed 单元素：只编码值，不写 tag（tag 在 begin 时已写） */
static void
_packed_push_int(struct pbc_wmessage *m, struct _field *f, uint32_t low, uint32_t hi) {
	struct pbc_wmessage *r = _wm_root(m);
	_expand_message(r, 10);
	switch (f->type) {
	case PTYPE_INT64:
	case PTYPE_UINT64:
	case PTYPE_INT32:
		r->ptr += _pbcV_encode((uint64_t)low | (uint64_t)hi << 32 , r->ptr);
		break;
	case PTYPE_UINT32:
	case PTYPE_ENUM:
	case PTYPE_BOOL:
		r->ptr += _pbcV_encode32(low, r->ptr);
		break;
	case PTYPE_FIXED64:
	case PTYPE_SFIXED64:
		int64_encode(low, hi, r->ptr);
		r->ptr += 8;
		break;
	case PTYPE_FIXED32:
	case PTYPE_SFIXED32:
		int32_encode(low, r->ptr);
		r->ptr += 4;
		break;
	case PTYPE_SINT32:
		r->ptr += _pbcV_zigzag32((int32_t)low, r->ptr);
		break;
	case PTYPE_SINT64:
		r->ptr += _pbcV_zigzag((int64_t)((uint64_t)low | (uint64_t)hi << 32), r->ptr);
		break;
	}
}

static void
_packed_push_real(struct pbc_wmessage *m, struct _field *f, double v) {
	struct pbc_wmessage *r = _wm_root(m);
	_expand_message(r, 10);
	switch (f->type) {
	case PTYPE_FLOAT:
		float_encode((float)v, r->ptr);
		r->ptr += 4;
		break;
	case PTYPE_DOUBLE:
		double_encode(v, r->ptr);
		r->ptr += 8;
		break;
	}
}

/* 按 key 解析一次字段，返回不透明句柄。同时收掉上一层未结束的子视图，
 * 这样「打开子 message -> 回到父层写下一个字段」不需要调用方显式收尾。
 * 句柄就是 env 拥有的 struct _field*，随 env 只读，可跨调用复用。 */
struct pbc_wfield *
pbc_wmessage_field(struct pbc_wmessage *m, const char *key) {
	if (m == NULL || key == NULL) {
		return NULL;
	}
	_close_open_sub(m);
	return (struct pbc_wfield *)_pbcM_sp_query(m->type->name, key);
}

int
pbc_wmessage_field_type(struct pbc_wfield *wf, const char **type) {
	if (wf == NULL) {
		return 0;
	}
	return _pbcP_type((struct _field *)wf, type);
}

int
pbc_wmessage_field_packed(struct pbc_wfield *wf) {
	return wf != NULL && ((struct _field *)wf)->label == LABEL_PACKED;
}

/* ---------------------------------------------------------------------------
 * 显式字段句柄写入
 * --------------------------------------------------------------------------- */
int
pbc_wmessage_integer_f(struct pbc_wmessage *m, struct pbc_wfield *wf, uint32_t low, uint32_t hi) {
	struct _field * f = (struct _field *)wf;
	if (f == NULL) {
		m->type->env->lasterror = "wmessage_integer invalid field";
		return -1;
	}
	struct pbc_wmessage *r = _wm_root(m);

	if (f->label == LABEL_PACKED) {
		if (m->packed_f != f) {
			_flush(m);
			_begin_packed(m, f);
		}
		_packed_push_int(m, f, low, hi);
		return 0;
	}
	_flush(m);

	if (f->label == LABEL_OPTIONAL) {
		if (f->type == PTYPE_ENUM) {
			if (low == f->default_v->e.id)
				return 0;
		} else {
			if (low == f->default_v->integer.low &&
				hi == f->default_v->integer.hi) {
				return 0;
			}
		}
	}
	int id = f->id << 3;

	_expand_message(r,20);
	switch (f->type) {
	case PTYPE_INT64:
	case PTYPE_UINT64:
	case PTYPE_INT32:
		id |= WT_VARINT;
		r->ptr += _pbcV_encode32(id, r->ptr);
		r->ptr += _pbcV_encode((uint64_t)low | (uint64_t)hi << 32 , r->ptr);
		break;
	case PTYPE_UINT32:
	case PTYPE_ENUM:
	case PTYPE_BOOL:
		id |= WT_VARINT;
		r->ptr += _pbcV_encode32(id, r->ptr);
		r->ptr += _pbcV_encode32(low, r->ptr);
		break;
	case PTYPE_FIXED64:
	case PTYPE_SFIXED64:
		id |= WT_BIT64;
		r->ptr += _pbcV_encode32(id, r->ptr);
		int64_encode(low,hi,r->ptr);
		r->ptr += 8;
		break;
	case PTYPE_FIXED32:
	case PTYPE_SFIXED32:
		id |= WT_BIT32;
		r->ptr += _pbcV_encode32(id, r->ptr);
		int32_encode(low,r->ptr);
		r->ptr += 4;
		break;
	case PTYPE_SINT32:
		id |= WT_VARINT;
		r->ptr += _pbcV_encode32(id, r->ptr);
		r->ptr += _pbcV_zigzag32((int32_t)low, r->ptr);
		break;
	case PTYPE_SINT64:
		id |= WT_VARINT;
		r->ptr += _pbcV_encode32(id, r->ptr);
		r->ptr += _pbcV_zigzag((int64_t)((uint64_t)low | (uint64_t)hi << 32), r->ptr);
		break;
	}

	return 0;
}

int
pbc_wmessage_real_f(struct pbc_wmessage *m, struct pbc_wfield *wf, double v) {
	struct _field * f = (struct _field *)wf;
	if (f == NULL) {
		m->type->env->lasterror = "wmessage_real invalid field";
		return -1;
	}
	struct pbc_wmessage *r = _wm_root(m);

	if (f->label == LABEL_PACKED) {
		if (m->packed_f != f) {
			_flush(m);
			_begin_packed(m, f);
		}
		_packed_push_real(m, f, v);
		return 0;
	}
	_flush(m);

	if (f->label == LABEL_OPTIONAL) {
		if (v == f->default_v->real)
			return 0;
	}
	int id = f->id << 3;
	_expand_message(r,18);
	switch (f->type) {
	case PTYPE_FLOAT: {
		id |= WT_BIT32;
		r->ptr += _pbcV_encode32(id, r->ptr);
		float_encode((float)v , r->ptr);
		r->ptr += 4;
		break;
	}
	case PTYPE_DOUBLE:
		id |= WT_BIT64;
		r->ptr += _pbcV_encode32(id, r->ptr);
		double_encode(v , r->ptr);
		r->ptr += 8;
		break;
	}

	return 0;
}

int
pbc_wmessage_string_f(struct pbc_wmessage *m, struct pbc_wfield *wf, const char * v, int len) {
	struct _field * f = (struct _field *)wf;
	if (f == NULL) {
		m->type->env->lasterror = "wmessage_string invalid field";
		return -1;
	}
	struct pbc_wmessage *r = _wm_root(m);

	bool varlen = false;

	if (len <=0) {
		varlen = true;
		// -1 for add '\0'
		len = strlen(v) - len;
	}
	if (f->label == LABEL_PACKED) {
		/* 只有 enum 名会以字符串形式写进 packed 字段 */
		if (f->type == PTYPE_ENUM) {
			char * temp = (char *)alloca(len + 1);
			if (!varlen || v[len] != '\0') {
				memcpy(temp,v,len);
				temp[len]='\0';
				v = temp;
			}
			int enum_id = 0;
			int err = _pbcM_si_query(f->type_name.e->name, v , &enum_id);
			if (err) {
				m->type->env->lasterror = "wmessage_string packed invalid enum";
				return -1;
			}
			if (m->packed_f != f) {
				_flush(m);
				_begin_packed(m, f);
			}
			_packed_push_int(m, f, (uint32_t)enum_id, 0);
		}
		return 0;
	}
	_flush(m);

	if (f->label == LABEL_OPTIONAL) {
		if (f->type == PTYPE_ENUM) {
			if (strncmp(v , f->default_v->e.name, len) == 0 && f->default_v->e.name[len] =='\0') {
				return 0;
			}
		} else if (f->type == PTYPE_STRING) {
			if (len == f->default_v->s.len &&
				strcmp(v, f->default_v->s.str) == 0) {
				return 0;
			}
		} else if (f->type == PTYPE_BYTES) {
			if (len == 0) {
				return 0;
			}
		}
	}
	int id = f->id << 3;
	_expand_message(r,20);
	switch (f->type) {
	case PTYPE_ENUM : {
		char * temp = (char *)alloca(len+1);
		if (!varlen || v[len] != '\0') {
			memcpy(temp,v,len);
			temp[len]='\0';
			v = temp;
		}
		int enum_id = 0;
		int err = _pbcM_si_query(f->type_name.e->name, v, &enum_id);
		if (err) {
			m->type->env->lasterror = "wmessage_string invalid enum";
			return -1;
		}
		id |= WT_VARINT;
		r->ptr += _pbcV_encode32(id, r->ptr);
		r->ptr += _pbcV_encode32(enum_id, r->ptr);
		break;
	}
	case PTYPE_STRING:
	case PTYPE_BYTES:
		id |= WT_LEND;
		r->ptr += _pbcV_encode32(id, r->ptr);
		r->ptr += _pbcV_encode32(len, r->ptr);
		_expand_message(r,len);
		memcpy(r->ptr , v , len);
		r->ptr += len;
		break;
	}

	return 0;
}

struct pbc_wmessage *
pbc_wmessage_message_f(struct pbc_wmessage *m, struct pbc_wfield *wf) {
	struct _field * f = (struct _field *)wf;
	if (f == NULL) {
		m->type->env->lasterror = "wmessage_message invalid field";
		return NULL;
	}
	struct pbc_wmessage *r = _wm_root(m);
	_flush(m);

	int id = f->id << 3 | WT_LEND;
	_expand_message(r, 20);
	r->ptr += _pbcV_encode32(id, r->ptr);
	int off = (int)(r->ptr - r->buffer);
	*r->ptr++ = 0;	/* 1 字节长度占位，end_sub 时按需要扩成多字节 varint */

	struct pbc_wmessage *v = (struct pbc_wmessage *)_pbcH_alloc(r->heap, sizeof(*v));
	v->type = f->type_name.m;
	v->root = r;
	v->heap = r->heap;
	v->buffer = NULL;
	v->ptr = NULL;
	v->endptr = NULL;
	v->len_off = off;
	v->packed_len_off = -1;
	v->packed_f = NULL;
	v->open_sub = NULL;
	v->closed = 0;
	m->open_sub = v;
	return v;
}

void
pbc_wmessage_end_sub(struct pbc_wmessage *v) {
	if (v == NULL || v->root == NULL || v->closed) {
		return;
	}
	_flush(v);	/* 递归收掉更深的视图和本层的 packed */
	_commit_len(v, v->len_off);
	v->closed = 1;
}

/* ---------------------------------------------------------------------------
 * packed 批量写入：encode_field_list 识别 packed 后一次 begin / 逐元素 push /
 * 一次 end，循环里不再走完整的字段类型分派与默认值判断。
 * --------------------------------------------------------------------------- */
int
pbc_wmessage_packed_begin(struct pbc_wmessage *m, struct pbc_wfield *wf) {
	struct _field *f = (struct _field *)wf;
	if (f == NULL || f->label != LABEL_PACKED) {
		return 0;
	}
	_flush(m);
	_begin_packed(m, f);
	return 1;
}

void
pbc_wmessage_packed_integer(struct pbc_wmessage *m, struct pbc_wfield *wf, uint32_t low, uint32_t hi) {
	struct _field *f = m->packed_f ? m->packed_f : (struct _field *)wf;
	if (f) {
		_packed_push_int(m, f, low, hi);
	}
}

void
pbc_wmessage_packed_real(struct pbc_wmessage *m, struct pbc_wfield *wf, double v) {
	struct _field *f = m->packed_f ? m->packed_f : (struct _field *)wf;
	if (f) {
		_packed_push_real(m, f, v);
	}
}

void
pbc_wmessage_packed_end(struct pbc_wmessage *m) {
	if (m->packed_len_off >= 0) {
		_commit_len(m, m->packed_len_off);
		m->packed_len_off = -1;
		m->packed_f = NULL;
	}
}

/* ---------------------------------------------------------------------------
 * 兼容层：按 key 写入。内部只多一次字段解析，pbc/binding/lua*、pbc/test
 * 以及引擎里已有的调用点都不需要改。
 * --------------------------------------------------------------------------- */
int
pbc_wmessage_integer(struct pbc_wmessage *m, const char *key, uint32_t low, uint32_t hi) {
	return pbc_wmessage_integer_f(m, pbc_wmessage_field(m, key), low, hi);
}

int
pbc_wmessage_real(struct pbc_wmessage *m, const char *key, double v) {
	return pbc_wmessage_real_f(m, pbc_wmessage_field(m, key), v);
}

int
pbc_wmessage_string(struct pbc_wmessage *m, const char *key, const char *v, int len) {
	return pbc_wmessage_string_f(m, pbc_wmessage_field(m, key), v, len);
}

struct pbc_wmessage *
pbc_wmessage_message(struct pbc_wmessage *m, const char *key) {
	return pbc_wmessage_message_f(m, pbc_wmessage_field(m, key));
}

void *
pbc_wmessage_buffer(struct pbc_wmessage *m, struct pbc_slice *slice) {
	struct pbc_wmessage *r = _wm_root(m);
	/* 递归收掉所有未显式 end 的层：lua binding 只把子视图当 lightuserdata
	 * 交给 Lua，不会调 end_sub，靠这里和下一次写同层字段自动收尾。 */
	_flush(r);
	slice->buffer = r->buffer;
	slice->len = r->ptr - r->buffer;
	return r->buffer;
}
