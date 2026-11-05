#ifndef PROTOBUF_C_ALLOC_H
#define PROTOBUF_C_ALLOC_H

#include <stdlib.h>
#include <string.h>

void * _pbcM_malloc(size_t sz);
void _pbcM_free(void *p);
void * _pbcM_realloc(void *p, size_t sz);
void _pbcM_memory();

struct heap;

struct heap * _pbcH_new(int pagesize);
void _pbcH_delete(struct heap *);
/* 保留首个 page（wmessage 对象与初始 buffer 都在其中），释放后续 page 并归零
 * used，使整个 heap 可以重新分配。供 pbc_wmessage_reset 复用 wmessage 用。 */
void _pbcH_reset(struct heap *);
void* _pbcH_alloc(struct heap *, int size);

#define HMALLOC(size) ((h) ? _pbcH_alloc(h, size) : _pbcM_malloc(size))

#define malloc _pbcM_malloc
#define free _pbcM_free
#define realloc _pbcM_realloc
#define memory _pbcM_memory

#ifdef _WIN32

#include <malloc.h>

#endif

#ifdef _MSC_VER

#define alloca _alloca

#endif

#endif
