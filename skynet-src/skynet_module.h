/**
 * 管理加载的动态链接库, 并且通过加载的动态链接库生成对应的实力.
 */

#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

/// 创建 C 服务模块实例函数接口声明
typedef void * (*skynet_dl_create)(void);

/// C 服务模块初始化函数接口声明
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);

/// C 服务模块资源释放函数接口声明
typedef void (*skynet_dl_release)(void * inst);

/// C 服务模块对于 signal(信号) 的特殊处理接口声明
typedef void (*skynet_dl_signal)(void * inst, int signal);

struct skynet_module {
	const char * name;             // 模块的名字
	void * module;                 // 加载的动态链接库
	skynet_dl_create create;       // 加载的动态链接库中的创建服务实例函数
	skynet_dl_init init;           // 加载的动态链接库中的初始化服务实例函数
	skynet_dl_release release;     // 加载的动态链接库中的释放服务实例资源函数
	skynet_dl_signal signal;       // 加载的动态链接库中的服务实例对信号量的处理函数
};

/// 插入新的 skynet_module 
void skynet_module_insert(struct skynet_module *mod);

/**
 * 根据 name 查询 skynet_module.
 * @return 如果查询不到返回 NULL.
 */
struct skynet_module * skynet_module_query(const char * name);

/// 创建 skynet_module 对应的服务实例
void * skynet_module_instance_create(struct skynet_module *);

/// 使用 skynet_module 初始化对应的 inst 实例
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);

/// 使用 skynet_module 释放对应的 inst 实例
void skynet_module_instance_release(struct skynet_module *, void *inst);

/// 使用 skynet_module 让 inst 实例对信号量做处理
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

/// 当前节点的 skynet_module 相关的初始化
void skynet_module_init(const char *path);

#endif
