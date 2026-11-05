#ifndef PROTOBUF_C_H
#define PROTOBUF_C_H

#include <stdio.h>
#include <stdint.h>

/* 语法版本在编译期确定：一个进程里注册的所有 .proto 必须用同一版本。
 *   默认（两个宏都不定义）                -> proto3：repeated 数值字段默认 packed
 *   定义 PBC_PROTO2（CMake -DPBC_PROTO2=ON）-> proto2：只有显式 [packed=true] 才 packed
 * pbc 不按文件保存 syntax，也没有 per-file 模式；这里用编译期常量取代运行时
 * 扫描 FileDescriptorSet.syntax，注册路径不再有额外的一趟 wire 扫描。
 * 注意：proto2 文件若在 proto3 构建下注册，其 repeated 数值字段也会按 packed 编码，
 * 因此混用 proto2/proto3 的工程必须自行选择版本（或统一为 proto3）。 */
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
#define PBC_FIXED64 7
#define PBC_FIXED32 8
#define PBC_BYTES 9
#define PBC_INT64 10
#define PBC_UINT 11
#define PBC_UNKNOWN 12
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

struct pbc_field {
	int id;
	const char *name;
};
int pbc_fields(struct pbc_env *, const char * type_name, struct pbc_field *fields, const int max_field);

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

/* ---------------------------------------------------------------------------
 * 解码扁平快速路径（给 C/C++ 直连 binding 用）
 *
 * 旧的 pbc_decode 是「解 tag -> 装进 atom -> 转 union pbc_value -> 回调 ->
 * 再 switch 推 Lua」，还要按 schema 字符串重查子 message。这里改成 core 只解
 * tag 与字段元数据，按字段类型直接从 slice 读一次值，一次回调交给 binding
 * rawset；字段元数据在注册收尾时预编译，子 message 用指针直达。
 * pbc core 本身不依赖 Lua，旧的回调版仍保留给 pbc/binding/lua* 使用；
 * 静态库链接时未被引用的那一份会被链接器剔除。
 * --------------------------------------------------------------------------- */
struct pbc_msg;	/* 不透明：core 内的 struct _message */

struct pbc_ffield {
	int id;
	int type;					/* PBC_*，含 PBC_REPEATED */
	const char *name;			/* interned 字段名 */
	const char *type_name;		/* enum/message 的类型名，其余为 NULL */
	struct pbc_msg *sub;		/* PBC_MESSAGE 时按指针直达子 message */
	int field_count;			/* 所属 message 的字段数，用于预分配 Lua 表 */
};

struct pbc_fvalue {
	uint64_t i;					/* 整数类（varint / fixed / zigzag 解出的值） */
	double f;					/* float / double */
	const char *s;				/* string / bytes / message 的数据指针 */
	int len;
	const char *ename;			/* enum 名 */
	int index;					/* packed 元素序号（1 起）；非 packed 为 0 */
	int count;					/* packed 元素总数；非 packed 为 0 */
};

/* 返回非 0 立即中止解码 */
typedef int (*pbc_fast_decoder)(void *ud, const struct pbc_ffield *field, const struct pbc_fvalue *v);

struct pbc_msg * pbc_msg_from_name(struct pbc_env *, const char *type_name);
int pbc_msg_field_count(struct pbc_msg *);
int pbc_decode_fast(struct pbc_env *, const char *type_name, struct pbc_slice *, pbc_fast_decoder, void *ud);
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

/* 兼容层：按 key 写入，内部解析一次字段句柄再转发到 *_f。
 * pbc/binding/lua*、pbc/test 与引擎里已有的调用点都不需要改动。 */
// for negative integer, pass -1 to hi
int pbc_wmessage_integer(struct pbc_wmessage *, const char *key, uint32_t low, uint32_t hi);
int pbc_wmessage_real(struct pbc_wmessage *, const char *key, double v);
int pbc_wmessage_string(struct pbc_wmessage *, const char *key, const char * v, int len);
struct pbc_wmessage * pbc_wmessage_message(struct pbc_wmessage *, const char *key);

/* 热路径：显式字段句柄。先按 key 解析一次，之后写这个字段（包括 repeated
 * 逐元素）都把句柄传回来，内部不再按 key 查表，也没有任何隐式状态。
 * 句柄内部即 env 拥有的 struct _field*，随 env 只读；env 热更时必须丢弃旧句柄。 */
struct pbc_wfield;
struct pbc_wfield * pbc_wmessage_field(struct pbc_wmessage *, const char *key);
int pbc_wmessage_field_type(struct pbc_wfield *, const char **type);
int pbc_wmessage_field_packed(struct pbc_wfield *);

int pbc_wmessage_integer_f(struct pbc_wmessage *, struct pbc_wfield *, uint32_t low, uint32_t hi);
int pbc_wmessage_real_f(struct pbc_wmessage *, struct pbc_wfield *, double v);
int pbc_wmessage_string_f(struct pbc_wmessage *, struct pbc_wfield *, const char * v, int len);
struct pbc_wmessage * pbc_wmessage_message_f(struct pbc_wmessage *, struct pbc_wfield *);

/* 子 message 共享父 wmessage 的 buffer：message_f 写 tag + 预留长度槽，
 * 编码完调用 end_sub 回写长度。不显式调用也可以，下一次写同层字段或
 * pbc_wmessage_buffer 会自动收尾，兼容只拿到 lightuserdata 的 lua binding。 */
void pbc_wmessage_end_sub(struct pbc_wmessage *);

/* packed 数组批量写入：一次 begin、逐元素 push、一次 end。
 * payload 直接进父 buffer，不再攒 pbc_array 再第二趟打包，也不需要
 * 每个 wmessage 都带一张按 field id 索引的表。 */
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
