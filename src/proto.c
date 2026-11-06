#include "pbc.h"
#include "proto.h"
#include "pattern.h"
#include "map.h"
#include "alloc.h"
#include "stringpool.h"
#include "bootstrap.h"
#include "context.h"

#include <stdlib.h>
#include <string.h>

const char *
pbc_error(struct pbc_env * p) {
	const char *err = p->lasterror;
	p->lasterror = "";
	return err;
}

struct _message *
_pbcP_get_message(struct pbc_env * p , const char *name) {
	return (struct _message *)_pbcM_sp_query(p->msgs, name);
}

struct pbc_env *
pbc_new(void) {
	struct pbc_env * p = (struct pbc_env *)malloc(sizeof(*p));
	p->files = _pbcM_sp_new(0 , NULL);
	p->enums = _pbcM_sp_new(0 , NULL);
	p->msgs = _pbcM_sp_new(0 , NULL);
	p->lasterror = "";

	_pbcB_init(p);
	/* 自举 descriptor 注册完毕，先把内置 message 的解码缓存建好 */
	_pbcP_seal(p);

	return p;
}

static void
free_enum(void *p) {
	struct _enum * e = (struct _enum *)p;
	_pbcM_ip_delete(e->id);
	_pbcM_si_delete(e->name);

	free(p);
}

static void
free_stringpool(void *p) {
	_pbcS_delete((struct _stringpool *)p);
}

static void
free_msg(void *p) {
	struct _message * m = (struct _message *)p;
	if (m->id)
		_pbcM_ip_delete(m->id);
	free(m->decode_cache);
	free(m->def);
	_pbcM_sp_foreach(m->name, free);
	_pbcM_sp_delete(m->name);
	free(p);
}

void
pbc_delete(struct pbc_env *p) {
	_pbcM_sp_foreach(p->enums, free_enum);
	_pbcM_sp_delete(p->enums);

	_pbcM_sp_foreach(p->msgs, free_msg);
	_pbcM_sp_delete(p->msgs);

	_pbcM_sp_foreach(p->files, free_stringpool);
	_pbcM_sp_delete(p->files);

	free(p);
}

struct _enum *
_pbcP_push_enum(struct pbc_env * p, const char *name, struct map_kv *table, int sz) {
	void * check = _pbcM_sp_query(p->enums, name);
	if (check)
		return NULL;
	struct _enum * v = (struct _enum *)malloc(sizeof(*v));
	v->key = name;
	v->id = _pbcM_ip_new(table,sz);
	v->name = _pbcM_si_new(table,sz);
	v->default_v->e.id = table[0].id;
	v->default_v->e.name = (const char *)table[0].pointer;

	_pbcM_sp_insert(p->enums, name , v);
	return v;
}

void
_pbcP_push_message(struct pbc_env * p, const char *name, struct _field *f , pbc_array queue) {
	struct _message * m = (struct _message *)_pbcM_sp_query(p->msgs, name);
	if (m==NULL) {
		m = (struct _message *)malloc(sizeof(*m));
		m->def = NULL;
		m->key = name;
		m->id = NULL;
		m->name = _pbcM_sp_new(0 , NULL);
		m->env = p;
		m->decode_cache = NULL;
		m->decode_cap = 0;
		m->field_count = 0;
		m->is_map_entry = 0;
		_pbcM_sp_insert(p->msgs, name, m);
	}
	struct _field * field = (struct _field *)malloc(sizeof(*field));
	memcpy(field,f,sizeof(*f));
	_pbcM_sp_insert(m->name, field->name, field);
	pbc_var atom;
	atom->m.buffer = field;
	if (f->type == PTYPE_MESSAGE || f->type == PTYPE_ENUM) {
		_pbcA_push(queue, atom);
	}
}

struct _iter {
	int count;
	struct map_kv * table;
};

static void
_count(void *p, void *ud) {
	struct _iter *iter = (struct _iter *)ud;
	iter->count ++;
}

static void
_set_table(void *p, void *ud) {
	struct _field * field = (struct _field *)p;
	struct _iter *iter = (struct _iter *)ud;
	iter->table[iter->count].id = field->id;
	iter->table[iter->count].pointer = field;
	++iter->count;
}

struct _message *
_pbcP_init_message(struct pbc_env * p, const char *name) {
	struct _message * m = (struct _message *)_pbcM_sp_query(p->msgs, name);
	if (m == NULL) {
		m = (struct _message *)malloc(sizeof(*m));
		m->def = NULL;
		m->key = name;
		m->id = NULL;
		m->name = _pbcM_sp_new(0 , NULL);
		m->env = p;
		m->decode_cache = NULL;
		m->decode_cap = 0;
		m->field_count = 0;
		m->is_map_entry = 0;
		_pbcM_sp_insert(p->msgs, name, m);

		return m;
	}
	if (m->id) {
		// extend message, delete old id map.
		_pbcM_ip_delete(m->id);
	}
	struct _iter iter = { 0, NULL };
	_pbcM_sp_foreach_ud(m->name, _count, &iter);
	iter.table = (struct map_kv *)malloc(iter.count * sizeof(struct map_kv));
	iter.count = 0;
	_pbcM_sp_foreach_ud(m->name, _set_table, &iter);

	m->id = _pbcM_ip_new(iter.table , iter.count);

	/* 追加注册（扩展字段）会让已有的解码缓存失效，由注册收尾的 _pbcP_seal 重建 */
	free(m->decode_cache);
	m->decode_cache = NULL;
	m->decode_cap = 0;

	free(iter.table);

	return m;
}

int
_pbcP_message_default(struct _message * m, const char * name, pbc_var defv) {
	struct _field * f= (struct _field *)_pbcM_sp_query(m->name, name);
	if (f==NULL) {
		// invalid key
		defv->p[0] = NULL;
		defv->p[1] = NULL;
		return -1;
	}
	*defv = *(f->default_v);
	return f->type;
}

int
_pbcP_type(struct _field * field, const char ** type) {
	if (field == NULL) {
		return 0;
	}
	int ret = 0;
	switch (field->type) {
	case PTYPE_DOUBLE:
	case PTYPE_FLOAT:
		ret = PBC_REAL;
		break;
	case PTYPE_INT64:
	case PTYPE_SINT64:
		ret = PBC_INT64;
		break;
	case PTYPE_INT32:
	case PTYPE_SINT32:
		ret = PBC_INT;
		break;
	case PTYPE_UINT32:
		/* 拆开是必须的：uint32 与 fixed32 都是无符号，而 sfixed32 是有符号。
		 * 三者以前共用 PBC_UINT / PBC_FIXED32，解码侧只能一律按 int32 推给 Lua，
		 * 结果 0x80000000 以上的值全变成负数。 */
		ret = PBC_UINT32;
		break;
	case PTYPE_UINT64:
		ret = PBC_UINT;
		break;
	case PTYPE_FIXED32:
		ret = PBC_FIXED32;
		break;
	case PTYPE_SFIXED32:
		ret = PBC_SFIXED32;
		break;
	case PTYPE_SFIXED64:
	case PTYPE_FIXED64:
		ret = PBC_FIXED64;
		break;
	case PTYPE_BOOL:
		ret = PBC_BOOL;
		break;
	case PTYPE_STRING:
		ret = PBC_STRING;
		break;
	case PTYPE_BYTES:
		ret = PBC_BYTES;
		break;
	case PTYPE_ENUM:
		ret = PBC_ENUM;
		/* seal 会遍历 env 里全部 message，跨文件依赖未解析时这里可能是 NULL */
		if (type) {
			*type = field->type_name.e ? field->type_name.e->key : NULL;
		}
		break;
	case PTYPE_MESSAGE:
		ret = PBC_MESSAGE;
		if (type) {
			*type = field->type_name.m ? field->type_name.m->key : NULL;
		}
		break;
	default:
		return 0;
	}
	if (field->label == LABEL_REPEATED ||
		field->label == LABEL_PACKED) {
		ret |= PBC_REPEATED;
	}

	return ret;
}

static int
_comp_dfield(const void *a, const void *b) {
	const struct pbc_dfield *x = (const struct pbc_dfield *)a;
	const struct pbc_dfield *y = (const struct pbc_dfield *)b;
	return x->pub.id - y->pub.id;
}

/* 为一个 message 预编译解码字段缓存。只能由 _pbcP_seal 在注册阶段调用：
 * _pbcB_register_fields 会把 message/enum 字段的 type_name.n（字符串）解析成
 * type_name.m/e（指针），在此之前 _pbcP_type 会解引用野指针。
 *
 * 不再按 max_id 开稀疏数组：容量 = 字段数，与字段号大小无关，也不会因为
 * 一个很大的 id 就整表退化。解码侧按 id 二分查找，字段数 < 32 时最多 5 次
 * 整数比较，全部落在 L1。同时把子 message 指针一起存进来：解码侧免查 schema，
 * 编码侧靠同一张表正向遍历字段。 */
static struct pbc_dfield *
_build_decode_cache(struct _message *m) {
	if (m->decode_cache)
		return m->decode_cache;
	if (m->id == NULL)
		return NULL;

	struct _iter iter = { 0, NULL };
	_pbcM_sp_foreach_ud(m->name, _count, &iter);
	int n = iter.count;
	m->field_count = n;
	if (n <= 0)
		return NULL;

	iter.table = (struct map_kv *)malloc(n * sizeof(struct map_kv));
	iter.count = 0;
	_pbcM_sp_foreach_ud(m->name, _set_table, &iter);

	struct pbc_dfield * cache = (struct pbc_dfield *)malloc(n * sizeof(struct pbc_dfield));
	int used = 0;
	int i;
	for (i = 0; i < n; i++) {
		struct _field * f = (struct _field *)iter.table[i].pointer;
		const char * type_name = NULL;
		int type = _pbcP_type(f, &type_name);
		if (type == 0)
			continue;
		cache[used].pub.id = f->id;
		cache[used].pub.type = type;
		cache[used].pub.name = f->name;
		cache[used].pub.type_name = type_name;
		cache[used].pub.sub = (f->type == PTYPE_MESSAGE && f->type_name.m)
			? (struct pbc_msg *)f->type_name.m : NULL;
		/* proto3 map：repeated 字段指向注册阶段识别出的 MapEntry 嵌套消息，
		 * binding 据此把 Lua kv table 与 MapEntry 数组互相转换。 */
		cache[used].pub.is_map = (cache[used].pub.sub &&
			f->label == LABEL_REPEATED &&
			((struct _message *)cache[used].pub.sub)->is_map_entry) ? 1 : 0;
		cache[used].f = f;
		++used;
	}
	free(iter.table);
	qsort(cache, used, sizeof(*cache), _comp_dfield);
	m->decode_cache = cache;
	m->decode_cap = used;
	return cache;
}

struct pbc_msg *
pbc_msg_from_name(struct pbc_env *p, const char *type_name) {
	return (struct pbc_msg *)_pbcP_get_message(p, type_name);
}

int
pbc_msg_field_count(struct pbc_msg *m) {
	return m ? ((struct _message *)m)->field_count : 0;
}

const char *
pbc_msg_key(struct pbc_msg *m) {
	return m ? ((struct _message *)m)->key : NULL;
}

/* schema 遍历用的两个只读访问器：字段表按 id 升序，容量只与字段数有关。
 * 注意 decode_cap <= field_count：_pbcP_type 返回 0 的字段（依赖未解析、
 * 未知类型）在 seal 阶段就被跳过了，遍历边界必须用 cap。 */
int
pbc_msg_field_cap(struct pbc_msg *m) {
	return m ? ((struct _message *)m)->decode_cap : 0;
}

const struct pbc_ffield *
pbc_msg_field_at(struct pbc_msg *m, int i) {
	struct _message *mm = (struct _message *)m;
	if (mm == NULL || mm->decode_cache == NULL || i < 0 || i >= mm->decode_cap) {
		return NULL;
	}
	return &mm->decode_cache[i].pub;
}

/* 注册收尾：把 env 里所有还没建缓存的 message 一次性建好。
 * 追加注册（扩展字段）会在 _pbcP_init_message 里清掉旧缓存，这里负责重建。
 * 整个过程只在加载 pb / 热更建新 env 时发生，解码侧完全只读。 */
static void
_seal_one(void *p, void *ud) {
	(void)ud;
	_build_decode_cache((struct _message *)p);
}

void
_pbcP_seal(struct pbc_env *p) {
	_pbcM_sp_foreach_ud(p->msgs, _seal_one, NULL);
}

int
pbc_type(struct pbc_env * p, const char * type_name , const char * key , const char ** type) {
	struct _message *m = _pbcP_get_message(p, type_name);
	if (m==NULL) {
		return 0;
	}
	if (key == NULL) {
		return PBC_NOEXIST;
	}
	struct _field * field = (struct _field *)_pbcM_sp_query(m->name, key);
	return _pbcP_type(field, type);
}

int
pbc_enum_id(struct pbc_env *env, const char *enum_type, const char *enum_name) {
	struct _enum *enum_map = (struct _enum *)_pbcM_sp_query(env->enums, enum_type);
	if(!enum_map) {
		return -1;
	}
	int32_t enum_id = 0;
	int err = _pbcM_si_query(enum_map->name, enum_name, &enum_id);
	if(err) {
		return -1;
	}
	return enum_id;
}