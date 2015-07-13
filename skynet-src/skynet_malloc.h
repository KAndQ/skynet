/**
 * 这个文件声明了内存使用的 hook 函数声明.
 */

#ifndef skynet_malloc_h
#define skynet_malloc_h

#include <stddef.h>

// 如果使用了 jemalloc 的话, 那么会在 malloc_hook.c 文件中找到函数的定义.
// 如果没有使用 jemalloc 的话, 那么使用的就是标准库的 malloc, 这里只是又做了一次声明而已.

#define skynet_malloc malloc
#define skynet_calloc calloc
#define skynet_realloc realloc
#define skynet_free free

void * skynet_malloc(size_t sz);
void * skynet_calloc(size_t nmemb,size_t size);
void * skynet_realloc(void *ptr, size_t size);
void skynet_free(void *ptr);
char * skynet_strdup(const char *str);
void * skynet_lalloc(void *ud, void *ptr, size_t osize, size_t nsize);	// use for lua

#endif
