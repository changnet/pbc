#ifndef PROTOBUF_C_PATTERN_H
#define PROTOBUF_C_PATTERN_H

#include "pbc.h"
#include "context.h"
#include "array.h"

// 结构体中的一个变量，对应message中的一个field
struct _pattern_field {
	int id;
	int offset;
	int ptype;
	int ctype;
	int label;
	pbc_var defv;
};

// 一个pattern对应一个结构体。只在c、C++中使用时，可以把proto中的一个message
// 中的数据直接解析到结构体中，或者直接把结构体打包成二进制流
// _pattern_field即结构体中的变量，这个field的id需要按顺序的，因为会使用bsearch
struct pbc_pattern {
	struct pbc_env * env;
	int count;
	struct _pattern_field f[1];
};

struct pbc_pattern * _pbcP_new(struct pbc_env * env, int n);
int _pbcP_unpack_packed(uint8_t *buffer, int size, int ptype, pbc_array array);

#endif
