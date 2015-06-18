#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

#include <stdint.h>
#include <stdlib.h>

struct skynet_context;
struct skynet_message;
struct skynet_monitor;

/**
 * 创建 skynet_context
 * @param name module name
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

// skynet_context 引用计数 -1
struct skynet_context * skynet_context_release(struct skynet_context *);

/**
 * 获得 skynet_context 的 handle
 * @param skynet_handel
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
 * 生成当前服务的请求的 session, 具体的可以看 skynet.h 的 skynet_send 函数, 有 session 的详细说明
 * @param skynet_context
 * @return session
 */
int skynet_context_newsession(struct skynet_context *);


struct message_queue * skynet_context_message_dispatch(struct skynet_monitor *, struct message_queue *, int weight);	// return next queue

// 当前节点中 context 的总量
int skynet_context_total();

// 
void skynet_context_dispatchall(struct skynet_context * context);	// for skynet_error output before exit

// 标记当前的 context 发生了 endless 现象(信息无限循环发送)
void skynet_context_endless(uint32_t handle);	// for monitor

// 初始化, 只在 skynet_main 中使用.
void skynet_globalinit(void);

// 资源释放, 只在 skynet_main 中使用.
void skynet_globalexit(void);

// 
void skynet_initthread(int m);

#endif
