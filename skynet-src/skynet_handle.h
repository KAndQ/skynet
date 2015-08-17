/**
 * skynet_handle 处理着 handle, skynet_context, name 三者之间的关系.
 * 简单来说可以从:
 * 1. handle 得到 skynet_context;
 * 2. skynet_context 得到 handle;
 * 3. name 得到 handle;
 */

#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

// reserve high 8 bits for remote id
<<<<<<< HEAD
// 保留高 8 位用来存储远程节点的 id
#define HANDLE_MASK 0xffffff        // 用于计算 handle 的值

 // 计算 handle 时的位偏移量
=======
#define HANDLE_MASK 0xffffff
>>>>>>> cloudwu/master
#define HANDLE_REMOTE_SHIFT 24

struct skynet_context;

// handle 是一个高 8 位表示 harbor, 其他 24 位表示其 index. 0 index 被保留.

/**
 * 注册一个 skynet_context 到当前的进程(节点)中
 * @param skynet_context 注册的 context
 * @return 返回注册的 skynet_context 的 handle !!!
 */
uint32_t skynet_handle_register(struct skynet_context *);

/**
 * 回收 handle, 即删掉 handle 对应的 context, 此 handle 又可重用
 * @param handle 准备回收的 handle
 * @return 收回成功返回 1, 否则返回 0
 */
int skynet_handle_retire(uint32_t handle);

/**
 * 获得 handle 对应的 skynet_context 对象, 同时该 context 的引用计数 +1
 * @param handle 待查询的 handle
 * @return skynet_context
 */
struct skynet_context * skynet_handle_grab(uint32_t handle);

/**
 * 回收所有的数据, 即删除所有的 context
 */
void skynet_handle_retireall();

/**
 * 通过 name 找到对应的 handle
 * @param name 名字
 * @return handle
 */
uint32_t skynet_handle_findname(const char * name);

/**
 * 将 name 和 handle 关联起来, 1 个 name 只能注册 1 次, 1 个 handle 能够注册多个名字
 * @param handle handle
 * @param name name
 * @return name
 */
const char * skynet_handle_namehandle(uint32_t handle, const char *name);

/**
 * 关于管理 skynet_context 这套机制的初始化
 * @param harbor skynet 的网络节点
 */
void skynet_handle_init(int harbor);

#endif
