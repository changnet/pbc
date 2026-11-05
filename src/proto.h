#ifndef PROTOBUFC_PROTO_H
#define PROTOBUFC_PROTO_H

#include "pbc.h"
#include "map.h"
#include "array.h"
#ifndef _MSC_VER
#include <stdbool.h>
#endif
#include <stddef.h>

struct map_ip;
struct map_si;
struct map_sp;
struct _message;
struct _enum;

#define LABEL_OPTIONAL 0
#define LABEL_REQUIRED 1
#define LABEL_REPEATED 2
#define LABEL_PACKED 3

struct _field {
	int id;
	const char *name;
	int type;
	int label;
	pbc_var default_v;
	union {
		const char * n;
		struct _message * m;
		struct _enum * e;
	} type_name;
};

/* 解码字段元数据。原来按 field id 直接开数组（cap = max_id+1）：protobuf 的
 * 字段号可以很大，一个 id=65535 的字段就要吃掉 2MB，而且大 id 消息整体退化成
 * 稀疏表。现在改成按 id 升序排列的紧凑数组，容量只和字段数有关，与 id 分布
 * 无关；解码时二分查找（字段数通常 < 32，全在 L1）。pub 是给 C++ binding 的
 * 只读视图，f 只在 core 内部使用。 */
struct pbc_dfield {
	struct pbc_ffield pub;
	struct _field * f;
};

struct _message {
	const char * key;
	struct map_ip * id;	// id -> _field
	struct map_sp * name;	// string -> _field
	struct pbc_rmessage * def;	// default message
	struct pbc_env * env;
	struct pbc_dfield * decode_cache;	// 按 field id 升序，NULL 表示未构建
	int decode_cap;		// 缓存元素个数（不是数组下标上限）
	int field_count;	// 字段数，供 binding 预分配 Lua 表
};

struct _enum {
	const char * key;
	struct map_ip * id;
	struct map_si * name;
	pbc_var default_v;
};

struct pbc_env {
	struct map_sp * files;	// string -> void *
	struct map_sp * enums;	// string -> _enum
	struct map_sp * msgs;	// string -> _message
	const char * lasterror;
};

struct _message * _pbcP_init_message(struct pbc_env * p, const char *name);
void _pbcP_push_message(struct pbc_env * p, const char *name, struct _field *f , pbc_array queue);
struct _enum * _pbcP_push_enum(struct pbc_env * p, const char *name, struct map_kv *table, int sz );
int _pbcP_message_default(struct _message * m, const char * name, pbc_var defv);
struct _message * _pbcP_get_message(struct pbc_env * p, const char *name);
int _pbcP_type(struct _field * field, const char **type);
/* 注册收尾：为 env 里所有 message 预编译解码字段缓存。
 * 调用之后 env 的 schema 部分只读，pbc_decode 不再写任何共享状态。 */
void _pbcP_seal(struct pbc_env * p);

#endif
