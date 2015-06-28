/**
 * C 服务模块的接口定义.
 * C 服务模块的管理操作.
 * skynet_module 并不是 create 这类函数创建的服务实例, 它只是定义了这些服务所需要实现的接口. 
 * 同时 skynet_module 保留着加载的动态链接库.
 */

#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

/// 创建 C 服务模块实例函数接口定义
typedef void * (*skynet_dl_create)(void);

/// C 服务模块初始化函数接口定义
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);

/// C 服务模块资源释放函数接口定义
typedef void (*skynet_dl_release)(void * inst);

/// C 服务模块对于 signal(信号) 的特殊处理
typedef void (*skynet_dl_signal)(void * inst, int signal);

struct skynet_module {
	const char * name;     // 模块的名字
	void * module;         // 加载的动态链接库
	skynet_dl_create create;       // 创建服务实例
	skynet_dl_init init;           // 初始化服务实例
	skynet_dl_release release;     // 释放服务实例资源
	skynet_dl_signal signal;       // 服务实例对信号量的处理
};

/// 插入新的 skynet_module 
void skynet_module_insert(struct skynet_module *mod);

/**
 * 根据 name 查询 skynet_module.
 * @return 如果查询不到返回 NULL.
 */
struct skynet_module * skynet_module_query(const char * name);

/// 创建服务实例
void * skynet_module_instance_create(struct skynet_module *);

/// inst 实例初始化
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);

/// inst 实例资源释放
void skynet_module_instance_release(struct skynet_module *, void *inst);

/// inst 实例处理 signal
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

/// 当前节点的 skynet_module 相关的初始化
void skynet_module_init(const char *path);

#endif
