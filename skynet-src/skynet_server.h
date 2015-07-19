/**
 * 这里实现了 skynet 的核心逻辑.
 * 每个 skynet_context 在互相的发送消息, 从而形成当前节点内的数据通信. 
 * 和每个 skynet 网络节点进行数据传输其实也是依靠 skynet_context.
 */

#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

#include <stdint.h>
#include <stdlib.h>

struct skynet_context;
struct skynet_message;
struct skynet_monitor;

/**
 * 创建 skynet_context
 * @param name 模块的名称
 * @param parm 初始化参数
 * @return 创建成功返回 skynet_context, 否则返回 NULL
 */
struct skynet_context * skynet_context_new(const char * name, const char * parm);

// skynet_context 引用计数 +1
void skynet_context_grab(struct skynet_context *);

// skynet_context 引用计数 +1, 同时会 context 的 total 数量减 1.
// 不要统计保留的 context, 因为 skynet 只有在 context 为 0 的时候终止(工作的线程被终止).
// 保留的 context 将在最后被释放.
void skynet_context_reserve(struct skynet_context *ctx);

/**
 * skynet_context 引用计数 -1, 如果当引用计数为 0, skynet_context 的内存资源将被释放
 * @param skynet_context 被 release 的 ctx
 * @return 如果 skynet_context 资源没有被释放则返回传入的 skynet_context, 否则返回 NULL
 */
struct skynet_context * skynet_context_release(struct skynet_context *);

/**
 * 获得 skynet_context 的 handle, 只读, 不允许直接操作 skynet_context 的 handle
 * @param skynet_context
 * @return handle
 */
uint32_t skynet_context_handle(struct skynet_context *);

/**
 * 将 message 压入到 handle 对应的 context 的队列中
 * @param handle context 的 handle
 * @param message 数据信息
 * @return 成功返回 0, 否则返回 -1
 */
int skynet_context_push(uint32_t handle, struct skynet_message *message);

/**
 * 将数据压入到 context 队列中
 * @param context skynet_context
 * @param msg 数据
 * @param sz 数据大小
 * @param source 发送源
 * @param type 消息类型, 具体查看 skynet.h PTYPE_*
 * @param session 会话 ID
 */
void skynet_context_send(struct skynet_context * context, void * msg, size_t sz, uint32_t source, int type, int session);

/**
 * 生成当前 skynet_context 的新 session
 * @param skynet_context 计算 session 使用的 context
 * @return session, 永远不会为 0, 也不会为负数.
 */
int skynet_context_newsession(struct skynet_context *);

/**
 * dispatch message_queue 里面的消息
 * @param skynet_monitor 
 * @param message_queue 当前 dispatch 的 queue
 * @param weight 权重, 决定 message_queue dispatch skynet_message 的次数
 * @return 下次 dispatch 的队列
 */
struct message_queue * skynet_context_message_dispatch(struct skynet_monitor *, struct message_queue *, int weight);	// return next queue

// 当前节点中 skynet_context 的总量
int skynet_context_total();

// for skynet_error output before exit
// 适用于在退出之前 skynet_error 的输出
// 将 skynet_context 的队列数据全部弹出, 并且将对应的消息作为参数进行回调
void skynet_context_dispatchall(struct skynet_context * context);

// for monitor
// 作用于监视器
// 标记当前的 context 发生了 endless 现象(进入死循环逻辑)
void skynet_context_endless(uint32_t handle);

// 进程启动时初始化, 只在 skynet_main 中使用.
void skynet_globalinit(void);

// 进程关闭时资源释放, 只在 skynet_main 中使用.
void skynet_globalexit(void);

/**
 * 初始化各个线程的私有变量
 * @param m 使用的的初始化的值, 默认存储的值是 -m
 */
void skynet_initthread(int m);

#endif
