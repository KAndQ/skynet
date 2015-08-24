#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

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

/*

这里介绍下 skynet 是如何保证 message_queue 的信息是顺序执行的, 不用考虑多个线程会对同一个 message_queue 进行操作, 这在 skynet 中是不会出现的.

首先要注意的是如果 in_global 为 1, skynet_mq_push 函数是不会把 message_queue 加入到 global_queue 中的.

skynet_context_new 中创建 message_queue 的时候, 就已经把 message_queue.in_global 标记为 1. 那么直到在 skynet_context_new 函数显示调用 skynet_globalmq_push 
之前可以随意的 skynet_mq_push 消息也没关系(不会 dispatch 消息), 因为其实这时 message_queue 并不在 global_queue 中.

在 skynet_context_message_dispatch 中处理 message_queue 时, 首先从 global_queue 中弹出 message_queue, 这时虽然 message_queue 确实从 global_queue 中弹出,
但是 in_global 标记却还没有设置为 0, 还是保持 1. 这时从 message_queue 中弹出 skynet_message, 直到 message_queue 为空的时候, 才会设置 in_global 标记为 0. 

从 global_queue 弹出后, in_global 为 1 时, skynet_mq_push 消息是不会将 message_queue 压入进 global_queue 中的, 所以这时 message_queue 只存在于当前处理 
skynet_message 的线程中, 只要 message_queue 不在 global_queue 中, 那么其他线程是无法获得此 message_queue 的, 这样就保证了 1 个 message_queue 同时只能被 1 个线程处理.

在处理完 skynet_message 之后, 如果 message_queue 还有数据, 这时再将 message_queue 压入到 global_queue 中, 这样就保证可以继续处理 message_queue 里的消息. 如果 message_queue 
没有数据了, in_global 为 0, 再次调用 skynet_mq_push, 将 message_queue 压入到 global_queue 中, 保证可以继续处理 message_queue.

*/


// 管理 skynet_message 的队列, 队列数据结构. message_queue 同时只会在 1 个线程中运行.
struct message_queue {
	struct spinlock lock;				// 线程安全锁
	uint32_t handle;					// 关联 skynet_context 的 handle
	int cap;							// 当前能存放消息的最大容量
	int head;							// 队列头部的索引
	int tail;							// 队列尾部的索引
	int release;						// 队列资源释放标记, 0 标记未释放, 1 标记为释放

	// 是否压入到 global_queue 中, 这个标记很重要, 它决定着 message_queue 中的消息顺序执行, 并且同时只有 1 个线程在处理着这个 message_queue.
	// 只有当 message_queue 中的 skynet_message 为空的时候, in_global 才会被标记为 0. 在每次向 message_queue 中 push 的时候, 一定会将 in_global 设置为 1.
	int in_global;

	int overload;						// 当前持有 skynet_message 的数量, 只有在超载时才会被赋值
	int overload_threshold;				// 超载的阈值, 每次超载发生的时候, 该阈值也会增大 2 倍
	struct skynet_message *queue;		// skynet_message 的数组
	struct message_queue *next;			// 当压入到全局队列的时候, 关联的下一个 message_queue
};

// 当前节点管理 message_queue 的队列, 链表数据结构
// 只有在 global_queue 里面的 message_queue 才能被 pop 出, 以供 skynet_context_message_dispatch 执行
struct global_queue {
	struct message_queue *head;		// 队列的首指针
	struct message_queue *tail;		// 队列的尾指针
	struct spinlock lock;			// 线程的安全锁
};

static struct global_queue *Q = NULL;

void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q = Q;

	// 保证线程安全
	SPIN_LOCK(q)

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

	SPIN_UNLOCK(q)
}

struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	// 保证线程安全
	SPIN_LOCK(q)

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
	SPIN_UNLOCK(q)

	return mq;
}

struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q)

	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_force_push to push it to global queue.
	// 当创建一个 message_queue 时(总是在服务的 create 和 init 之间), 设置 in_global 标记避免压入到全局队列.
	// 如果服务初始化成功, skynet_context_new 方法将调用 skynet_globalmq_push 方法, 将它压入到全局队列.
	// 所以在 skynet_context 初始化时, 还没有调用 skynet_globalmq_push 方法前无论 push 进来多少的 skynet_message 都不会被 dispatch 掉.
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

	SPIN_DESTROY(q)

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

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)
	
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
	SPIN_LOCK(q)

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

	// 当 message_queue 为空时, 标记 message_queue 不在全局队列中, 因为只要还能够从 message_queue 中拿出 skynet_message,
	// 那么则认为 message_queue 还是在 global_message 中的.
	if (ret) {
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

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
	SPIN_LOCK(q)

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

	// 如果不在 global_queue 中, 则压入到 global_queue 中, 只要 message_queue 不为空, 则认为是在 global_queue 中.
	if (q->in_global == 0) {
		// 标记当前 q 在全局队列中
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}

void 
skynet_mq_init() {
	// 初始化 global_queue
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q, 0, sizeof(*q));
	SPIN_INIT(q);
	Q = q;
}

void 
skynet_mq_mark_release(struct message_queue *q) {

	// 保证线程安全
	SPIN_LOCK(q)

	// 之前没有被标记为释放
	assert(q->release == 0);

	// 标记当前需要释放
	q->release = 1;

	// 如果 message_queue(q) 不在 global_queue 中, 那么需要把 message_queue(q) 压入到 global_queue 中.
	// 
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}

	SPIN_UNLOCK(q)
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
	SPIN_LOCK(q)
	
	// 如果 q 已经标记了需要释放, 那么将释放掉 q 所有相关联的资源
	if (q->release) {
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud);

	// 如果 q 没有标记需要释放, 只将 q 压入到 global_queue 中去, 等待打了标记的时候才会执行删除
	} else {
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
