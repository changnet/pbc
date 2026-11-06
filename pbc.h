#ifndef PROTOBUF_C_H
#define PROTOBUF_C_H

#include <stdio.h>
#include <stdint.h>

/* 语法版本在编译期确定：一个进程里注册的所有 .proto 必须用同一版本。
 *   默认（两个宏都不定义）                -> proto3：repeated 数值字段默认 packed
 *   定义 PBC_PROTO2（CMake -DPBC_PROTO2=ON）-> proto2：只有显式 [packed=true] 才 packed
 */
#if defined(PBC_PROTO2) && defined(PBC_PROTO3)
#error "PBC_PROTO2 and PBC_PROTO3 are mutually exclusive"
#endif
#if !defined(PBC_PROTO2) && !defined(PBC_PROTO3)
#define PBC_PROTO3 1    /* 默认 proto3 */
#endif
#if defined(PBC_PROTO3)
#define PBC_SYNTAX_PROTO3 1
#else
#define PBC_SYNTAX_PROTO3 0
#endif

#define PBC_ARRAY_CAP 64

#define PBC_NOEXIST -1
#define PBC_INT 1
#define PBC_REAL 2
#define PBC_BOOL 3
#define PBC_ENUM 4
#define PBC_STRING 5
#define PBC_MESSAGE 6
#define PBC_FIXED64 7	/* fixed64 / sfixed64：Lua 只有有符号 64 位，无法区分 */
#define PBC_FIXED32 8	/* fixed32：无符号 */
#define PBC_BYTES 9
#define PBC_INT64 10	/* int64 / sint64 */
#define PBC_UINT 11	/* uint64 */
#define PBC_UNKNOWN 12
#define PBC_SFIXED32 13	/* sfixed32：有符号 */
#define PBC_UINT32 14	/* uint32：无符号，Lua 整数装得下 0~4294967295 */
#define PBC_REPEATED 128

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _pbc_array { char _data[PBC_ARRAY_CAP]; } pbc_array[1];

struct pbc_slice {
	void *buffer;
	int len;
};

struct pbc_pattern;
struct pbc_env;
struct pbc_rmessage;
struct pbc_wmessage;

struct pbc_env * pbc_new(void);
void pbc_delete(struct pbc_env *);
int pbc_register(struct pbc_env *, struct pbc_slice * slice);
int pbc_type(struct pbc_env *, const char * type_name , const char * key , const char ** type);
const char * pbc_error(struct pbc_env *);

// callback api
union pbc_value {
	struct {
		uint32_t low;
		uint32_t hi;
	} i;
	double f;
	struct pbc_slice s;
	struct {
		int id;
		const char * name;
	} e;
};

typedef void (*pbc_decoder)(void *ud, int type, const char * type_name, union pbc_value *v, int id, const char *key);
int pbc_decode(struct pbc_env * env, const char * type_name , struct pbc_slice * slice, pbc_decoder f, void *ud);

struct pbc_msg;	/* 不透明：core 内的 struct _message */

/* 一个字段的只读元数据。binding 编码时按它正向遍历 schema，解码时按 id 反查。
 * 字段句柄（struct pbc_wfield）也是它所属记录的 pbc_dfield::f，不需要额外解析。 */
struct pbc_ffield {
	int id;
	int type;					/* PBC_*，含 PBC_REPEATED */
	const char *name;			/* interned 字段名 */
	const char *type_name;		/* enum/message 的类型名，其余为 NULL */
	struct pbc_msg *sub;		/* PBC_MESSAGE 时按指针直达子 message */
	int is_map;					/* proto3 map：repeated MapEntry，binding 需做 kv 转换 */
};

struct pbc_fvalue {
	uint64_t i;					/* 整数类（varint / fixed / zigzag 解出的值） */
	double f;					/* float / double */
	const char *s;				/* string / bytes / message 的数据指针 */
	int len;
	int count;					/* packed 元素总数；非 packed 为 0 */
};

/* 返回非 0 立即中止解码 */
typedef int (*pbc_fast_decoder)(void *ud, const struct pbc_ffield *field, const struct pbc_fvalue *v);

struct pbc_msg * pbc_msg_from_name(struct pbc_env *, const char *type_name);
int pbc_msg_field_count(struct pbc_msg *);		/* schema 里的字段数（含无法解码的） */
const char * pbc_msg_key(struct pbc_msg *);		/* message 全名，用于报错 */
/* seal 阶段预编译的字段表：按 field id 升序，只读。
 * 编码侧靠它遍历 schema，不需要再按名字哈希。 */
int pbc_msg_field_cap(struct pbc_msg *);
const struct pbc_ffield * pbc_msg_field_at(struct pbc_msg *, int i);
int pbc_decode_fast_msg(struct pbc_msg *, struct pbc_slice *, pbc_fast_decoder, void *ud);

// message api

struct pbc_rmessage * pbc_rmessage_new(struct pbc_env * env, const char * type_name , struct pbc_slice * slice);
void pbc_rmessage_delete(struct pbc_rmessage *);

uint32_t pbc_rmessage_integer(struct pbc_rmessage * , const char *key , int index, uint32_t *hi);
double pbc_rmessage_real(struct pbc_rmessage * , const char *key , int index);
const char * pbc_rmessage_string(struct pbc_rmessage * , const char *key , int index, int *sz);
struct pbc_rmessage * pbc_rmessage_message(struct pbc_rmessage *, const char *key, int index);
int pbc_rmessage_size(struct pbc_rmessage *, const char *key);
int pbc_rmessage_next(struct pbc_rmessage *, const char **key);

struct pbc_wmessage * pbc_wmessage_new(struct pbc_env * env, const char *type_name);
void pbc_wmessage_delete(struct pbc_wmessage *);
/* 复用 wmessage：重置内部 buffer / packed / sub / 字段缓存，并把 type 换成新的
 * message。保留 heap（只留首 page），省掉每次编码的 malloc/free。
 * 返回重置后的 wmessage（可能与传入指针相同）；type_name 不存在时返回 NULL，
 * 此时原 wmessage 已被重置，调用方应改走 pbc_wmessage_new。
 * 注意：返回的 slice.buffer 指针在下一次 reset / delete 后失效，与原 encode_to_buffer 语义一致。 */
struct pbc_wmessage * pbc_wmessage_reset(struct pbc_wmessage *, const char *type_name);
/* 已经拿到 message 指针时用这个版本：省掉一次按名字哈希表查询。
 * 编码路径上每个包都要 reset，热路径上值得分开。 */
struct pbc_wmessage * pbc_wmessage_reset_msg(struct pbc_wmessage *, struct pbc_msg *);


int pbc_wmessage_integer(struct pbc_wmessage *, const char *key, uint32_t low, uint32_t hi);
int pbc_wmessage_real(struct pbc_wmessage *, const char *key, double v);
int pbc_wmessage_string(struct pbc_wmessage *, const char *key, const char * v, int len);
struct pbc_wmessage * pbc_wmessage_message(struct pbc_wmessage *, const char *key);

struct pbc_wfield;
struct pbc_wfield * pbc_wmessage_field(struct pbc_wmessage *, const char *key);
/* 由 pbc_ffield 直接取字段句柄：schema 遍历时手上只有只读元数据，
 * 这一步免掉按名字查询。 */
struct pbc_wfield * pbc_wmessage_field_at(struct pbc_wmessage *, const struct pbc_ffield *);

int pbc_wmessage_integer_f(struct pbc_wmessage *, struct pbc_wfield *, uint32_t low, uint32_t hi);
int pbc_wmessage_real_f(struct pbc_wmessage *, struct pbc_wfield *, double v);
int pbc_wmessage_string_f(struct pbc_wmessage *, struct pbc_wfield *, const char * v, int len);
struct pbc_wmessage * pbc_wmessage_message_f(struct pbc_wmessage *, struct pbc_wfield *);

/* 子 message 共享父 wmessage 的 buffer：message_f 写 tag + 预留长度槽，
 * 编码完调用 end_sub 回写长度
 */
void pbc_wmessage_end_sub(struct pbc_wmessage *);

/* packed 数组批量写入：一次 begin、逐元素 push、一次 end。
 * payload 直接进父 buffer，不再攒 pbc_array 再第二趟打包，也不需要
 * 每个 wmessage 都带一张按 field id 索引的表。
 */
int pbc_wmessage_packed_begin(struct pbc_wmessage *, struct pbc_wfield *);
void pbc_wmessage_packed_integer(struct pbc_wmessage *, struct pbc_wfield *, uint32_t low, uint32_t hi);
void pbc_wmessage_packed_real(struct pbc_wmessage *, struct pbc_wfield *, double v);
void pbc_wmessage_packed_end(struct pbc_wmessage *);

void * pbc_wmessage_buffer(struct pbc_wmessage *, struct pbc_slice * slice);

// array api

int pbc_array_size(pbc_array);
uint32_t pbc_array_integer(pbc_array array, int index, uint32_t *hi);
double pbc_array_real(pbc_array array, int index);
struct pbc_slice * pbc_array_slice(pbc_array array, int index);

void pbc_array_push_integer(pbc_array array, uint32_t low, uint32_t hi);
void pbc_array_push_slice(pbc_array array, struct pbc_slice *);
void pbc_array_push_real(pbc_array array, double v);

struct pbc_pattern * pbc_pattern_new(struct pbc_env * , const char * message, const char *format, ...);
void pbc_pattern_delete(struct pbc_pattern *);

// return unused bytes , -1 for error
int pbc_pattern_pack(struct pbc_pattern *, void *input, struct pbc_slice * s);

// <0 for error
int pbc_pattern_unpack(struct pbc_pattern *, struct pbc_slice * s , void * output);

void pbc_pattern_set_default(struct pbc_pattern * , void *data);
void pbc_pattern_close_arrays(struct pbc_pattern *, void *data);

int pbc_enum_id(struct pbc_env *env, const char *enum_type, const char *enum_name);

#ifdef __cplusplus
}
#endif

#endif
