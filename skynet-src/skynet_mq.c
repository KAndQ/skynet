#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64		// 队列能够存放消息的初始化大小
#define MAX_GLOBAL_MQ 0x10000		// 目前没有使用

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.
// 0 表示 mq 不是全局的 mq;
// 1 表示 mq 是全局的 mq, 或者消息正在派发.
#define MQ_IN_GLOBAL 1

// 超载的警告线
#define MQ_OVERLOAD 1024

// 管理 skynet_message 的队列, 循环队列数据结构
struct message_queue {
	uint32_t handle;	// 关联 skynet_context 的 handle
	int cap;			// 当前能存放消息的最大容量
	int head;			// 队列头部的索引
	int tail;			// 队列尾部的索引
	int lock;			// 线程安全锁
	int release;		// 队列资源释放标记, 0 标记未释放, 1 标记为释放
	int in_global;		// 是否压入到 global_queue 中
	int overload;				// 当前持有 skynet_message 的数量, 只有在超载时才会被赋值
	int overload_threshold;		// 超载的阈值, 每次超载发生的时候, 该阈值也会增大 2 倍
	struct skynet_message *queue;	// skynet_message 的数组
	struct message_queue *next;		// 当压入到全局队列的时候, 关联的下一个 message_queue
};

// 当前节点管理 message_queue 的队列, 链表数据结构
struct global_queue {
	struct message_queue *head;		// 队列的首指针
	struct message_queue *tail;		// 队列的尾指针
	int lock;		// 线程的安全锁
};

static struct global_queue *Q = NULL;

// 保证线程安全的一些处理
#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}
#define UNLOCK(q) __sync_lock_release(&(q)->lock);

void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q = Q;

	// 保证线程安全
	LOCK(q)

	// 保证 queue 之前并没有加入到列表中
	assert(queue->next == NULL);

	// 如果已经存在尾巴, 则在当前的尾巴之后压入 queue, 使之成为新的尾巴
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;

	// 压入第一个 queue
	} else {
		q->head = q->tail = queue;
	}

	UNLOCK(q)
}

struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	// 保证线程安全
	LOCK(q)

	struct message_queue *mq = q->head;
	if(mq) {

		// 将队首转交给原来的第二个元素充当现在的队首
		q->head = mq->next;

		// 如果已经没有元素
		if(q->head == NULL) {

			// 保证被弹出的 mq 和 队尾的 mq 是同一个
			assert(mq == q->tail);

			// 当前 global_queue 为空
			q->tail = NULL;
		}

		// 保证 mq 已经不在链表中
		mq->next = NULL;
	}
	UNLOCK(q)

	return mq;
}

struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	q->lock = 0;
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_force_push to push it to global queue.
	// 当创建一个队列时(总是在服务的 create 和 init 之间), 设置 in_global 标记避免压入到全局队列.
	// 如果服务初始化成功, skynet_context_new 方法将调用 skynet_globalmq_push 方法, 将它压入到全局队列.
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

/// 释放 message_queue 的资源, 但是 skynet_message 的资源释放不是由它负责.
static void 
_release(struct message_queue *q) {
	// 保证传入的 q 已经不在 global_queue 中
	assert(q->next == NULL);

	// 释放资源
	skynet_free(q->queue);
	skynet_free(q);
}

uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

int
skynet_mq_length(struct message_queue *q) {
	int head, tail, cap;

	LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	UNLOCK(q)
	
	// 计算长度
	if (head <= tail) {
		return tail - head;
	}
	return tail + cap - head;
}

int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;

		// 会重置超载量
		q->overload = 0;

		return overload;
	} 
	return 0;
}

int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;

	// 保证线程安全
	LOCK(q)

	// 保证 q 不是空队列
	if (q->head != q->tail) {

		// 获得队首元素
		*message = q->queue[q->head++];

		// 设置返回值
		ret = 0;

		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		// 保证索引在可用的区间内
		if (head >= cap) {
			q->head = head = 0;
		}

		// 计算当前队列的长度
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}

		// 超载阈值设置
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		// reset overload_threshold when queue is empty
		// 如果 queue 为空的时候, 重置 overload_threshold 的值
		q->overload_threshold = MQ_OVERLOAD;
	}

	// 当 q 为空时, 标记 q 不在全局队列中
	if (ret) {
		q->in_global = 0;
	}
	
	UNLOCK(q)

	return ret;
}

/// 扩展队列
static void
expand_queue(struct message_queue *q) {
	// 分配新的内存空间
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);

	// 将原来队列的数据复制到新的队列中
	int i;
	for (i=0; i < q->cap; i++) {
		// 新队列将从 0 索引作为 head
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	
	// 释放原来的内存空间, 保留新的内存空间
	skynet_free(q->queue);
	q->queue = new_queue;
}

void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);

	// 保证线程安全
	LOCK(q)

	// 将 message 压入队尾
	// 注意, 这里是赋值!!!
	q->queue[q->tail] = *message;

	// 保证索引在可用的区间内
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	// 保证有足够的空间可用
	if (q->head == q->tail) {
		expand_queue(q);
	}

	// 如果不在 global_queue 中, 则压入到 global_queue 中
	if (q->in_global == 0) {
		// 标记当前 q 在全局队列中
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	UNLOCK(q)
}

void 
skynet_mq_init() {
	// 初始化 global_queue
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q, 0, sizeof(*q));
	Q = q;
}

void 
skynet_mq_mark_release(struct message_queue *q) {

	// 保证线程安全
	LOCK(q)

	// 之前没有被标记为释放
	assert(q->release == 0);

	// 标记当前需要释放
	q->release = 1;

	// 如果 q 不在 global_queue 中, 那么需要把 q 压入到 global_queue 中
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}

	UNLOCK(q)
}

/**
 * 释放掉 q 的资源, 其管理的 skynet_message 资源由 drop_func 负责处理
 * @param q 被释放的 message_queue
 * @param drop_func 处理 skynet_message 资源的函数
 * @param ud 传给 drop_func 的第二个参数
 */
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;

	// 将其管理的所有 skynet_message 弹出, 并对 skynet_message 执行 drop_func
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}

	// 释放掉 message_queue 资源
	_release(q);
}

void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {

	// 保证线程安全
	LOCK(q)
	
	// 如果 q 已经标记了需要释放, 那么将释放掉 q 所有相关联的资源
	if (q->release) {
		UNLOCK(q)
		_drop_queue(q, drop_func, ud);

	// 如果 q 没有标记需要释放, 只将 q 压入到 global_queue 中去
	} else {
		skynet_globalmq_push(q);
		UNLOCK(q)
	}
}
