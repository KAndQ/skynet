/**
 * 实现 message_queue 的管理.
 * 内部有一个 global_queue, 负责管理 message_queue.
 * message_queue 和 skynet_context 是 1 对 1 的关系, global_queue 和 message_queue 是 1 对多的关系, 
 * 当前 skynet_context 成功初始化完成之后会将 context 的 queue 连接到 global_queue 链表上. 
 */

#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

// skynet 内部传输数据, 在各个 skynet_context 之间.
struct skynet_message {
	uint32_t source;       // 发送源
	int session;           // session, 细节查看 skynet.h
	void * data;           // 数据内容
	size_t sz;             // 数据内容大小, 高 8 位存的是 PTEXT_*, 请查看 skynet.h
};

// type is encoding in skynet_message.sz high 8 bit
// 类型编码在 skynet_message.sz 的高 8 位.
#define MESSAGE_TYPE_MASK (SIZE_MAX >> 8)
#define MESSAGE_TYPE_SHIFT ((sizeof(size_t)-1) * 8)

struct message_queue;

/**
 * 将 message_queue 压入到 global_queue 队列中
 * @param queue 准备压入的 message_queue
 */
void skynet_globalmq_push(struct message_queue * queue);

/// 从 global_queue 队首弹出一个 message_queue
struct message_queue * skynet_globalmq_pop(void);

/**
 * 创建 message_queue
 * @param handle 关联的 skynet_context 的 handle
 * @return message_queue
 */
struct message_queue * skynet_mq_create(uint32_t handle);

/// 将 message_queue 标记为需要释放
void skynet_mq_mark_release(struct message_queue *q);

/// 释放 skynet_message 的接口声明
typedef void (*message_drop)(struct skynet_message *, void *);

/// 释放 message_queue 的全部资源, 如果 message_queue 之前已经标记了 release, 那么所有资源都将被释; 否则压入到 global_queue 中
void skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud);

/// 得到 message_queue 的 handle
uint32_t skynet_mq_handle(struct message_queue *);

// 0 for success
/**
 * 从 message_queue 队首弹出一个 skynet_message 消息
 * 注意: 只有在弹出消息的时候才会做过载的计算
 * @param q message_queue
 * @param message 弹出的消息, 如果弹出成功, 给 message 赋值
 * @return 成功返回 0, 否则返回 1
 */
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);

/// 将 skynet_message 压入到 message_queue 中
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);

// return the length of message queue, for debug
// 得到 message_queue 的长度, 用于测试
int skynet_mq_length(struct message_queue *q);

// 得到当前 message_queue 的超载量.
// 注意, 如果当超载量不为 0 的时候, 调用这个函数会将超载量设置为 0.
int skynet_mq_overload(struct message_queue *q);

/// 当前节点的 global_queue 初始化
void skynet_mq_init();

#endif
