/*
该服务的主要功能: 与各个 slave 节点的通信由这个模块处理.
在更底层处理着和各个 slave 节点的关系, 同时接收来自 .slave 服务传来的消息间接的处理与 master 的关系.
各个 slave 发来的消息, 该服务会视情况将数据转发给 .slave 服务.
 */

#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_socket.h"
#include "skynet_handle.h"

/*
	harbor listen the PTYPE_HARBOR (in text)
	harbor 以文本的形式侦听 PTYPE_HARBOR 类型的消息

	N name : update the global name
	S fd id: connect to new harbor, we should send self_id to fd first , and then recv a id (check it), and at last send queue.
	A fd id: accept new harbor, we should send self_id to fd , and then send queue.

	N name : 更新全局名字
	S fd id: 连接到新的 harbor, 我们首先应该发送自己的 id 到 fd, 然后接收 1 个 id(检查它), 然后在最后发送队列.
	A fd id: 接收新的 harbor, 我们应该发送自己的 id 到 fd, 然后发送队列.

	If the fd is disconnected, send message to slave in PTYPE_TEXT.  D id
	If we don't known a globalname, send message to slave in PTYPE_TEXT. Q name

	如果 fd 断开连接, 以 PTYPE_TEXT 消息类型发送消息到 slave. D id
	如果我们不知道 1 个全局名字, 以 PTYPE_TEXT 消息类型发送消息给 slave. Q name
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#define HASH_SIZE 4096
#define DEFAULT_QUEUE_SIZE 1024

// 12 is sizeof(struct remote_message_header)
// 12 是 sizeof(struct remote_message_header)
#define HEADER_COOKIE_LENGTH 12

/*
	message type (8bits) is in destination high 8bits
	harbor id (8bits) is also in that place , but remote message doesn't need harbor id.

	消息类型(8 位)在 destination 的高 8 位保存,
	虽然 harbor id 也是在那个地方, 但是远程消息不需要 harbor id.

	远程消息的消息头
 */
struct remote_message_header {
	uint32_t source;		// 消息源, 高 8 位存储的是 harbor id
	uint32_t destination;	// 目标源, 高 8 位存储消息类型
	uint32_t session;		// session
};

// harbor 消息
struct harbor_msg {
	struct remote_message_header header;	// 消息头
	void * buffer;	// 数据内容
	size_t size;	// 数据大小
};

// 管理 harbor_msg 的队列
struct harbor_msg_queue {
	int size;	// 队列的容量大小
	int head;	// 队列头
	int tail;	// 队列尾
	struct harbor_msg * data;	// harbor_msg 数据指针
};

/// hashmap 中的数据元素
struct keyvalue {
	struct keyvalue * next;				// 关联的下一个 keyvalue 节点, 栈结构, 最新插入的在头
	char key[GLOBALNAME_LENGTH];		// 存储的字符串
	uint32_t hash;						// 计算出来的 hash 值
	uint32_t value;						// skynet_context handle
	struct harbor_msg_queue * queue;	// 当 keyvalue 已经创建, 但是 value == 0 时, 发送数据会压入到这个队列中, 一般是在查询全局名字时压入
};

/// 以 hash 表的数据结构存储 keyvalue
struct hashmap {
	struct keyvalue *node[HASH_SIZE];	// keyvalue 的集合
};

// slave status
#define STATUS_WAIT 0		// 目前没有使用
#define STATUS_HANDSHAKE 1	// 握手, 握手后进入 STATUS_HEADER 状态
#define STATUS_HEADER 2		// 读取数据头
#define STATUS_CONTENT 3	// 读取实际的数据内容
#define STATUS_DOWN 4		// slave 无效, 还未使用

/// 连接的其他节点
struct slave {
	int fd;								// socket id
	struct harbor_msg_queue *queue;		// 当还未与当前节点完成握手时, 发送的消息存储到这个队列中
	int status;							// 状态, 对应上面的宏定于
	int length;							// 需要读取的数据长度
	int read;							// 记录已经读取的数据长度
	uint8_t size[4];					// 存储数据头数据
	char * recv_buffer;					// 记录当次从 socket 读取的数据内容, 当完整的数据内容读取完, 会将数据发送给本地的 slave 服务
};

/// 当前 skynet 的节点数据结构
struct harbor {
	struct skynet_context *ctx;		// skynet_context
	int id;							// 当前节点 harbor id
	uint32_t slave;					// skynet_context handle
	struct hashmap * map;			// 存储服务的全局名字和 handle
	struct slave s[REMOTE_MAX];		// 与当前节点连接的 slave, 如果 slave.status 不为 STATUS_DOWN 时, 才认为该 slave 为可用的
};

// hash table
// hash 表

/// 向 harbor_msg_queue 里面压入 harbor_msg
static void
push_queue_msg(struct harbor_msg_queue * queue, struct harbor_msg * m) {
	// If there is only 1 free slot which is reserved to distinguish full/empty
	// of circular buffer, expand it.
	// 如果当只剩下 1 个空的缓存槽的时候, 扩展它.
	if (((queue->tail + 1) % queue->size) == queue->head) {
		// 分配新的内存
		struct harbor_msg * new_buffer = skynet_malloc(queue->size * 2 * sizeof(struct harbor_msg));

		// 将原来的数据复制给新分配的内存空间
		int i;
		for (i=0;i<queue->size-1;i++) {
			new_buffer[i] = queue->data[(i+queue->head) % queue->size];
		}

		// 释放原来的内存
		skynet_free(queue->data);

		// 记录新的队列数据
		queue->data = new_buffer;
		queue->head = 0;
		queue->tail = queue->size - 1;
		queue->size *= 2;
	}

	// 将 harbor_msg 压入到队列
	struct harbor_msg * slot = &queue->data[queue->tail];
	*slot = *m;
	queue->tail = (queue->tail + 1) % queue->size;
}

/// 将数据转换成 harbor_msg 压入到 harbor_msg_queue 中
static void
push_queue(struct harbor_msg_queue * queue, void * buffer, size_t sz, struct remote_message_header * header) {
	struct harbor_msg m;
	m.header = *header;
	m.buffer = buffer;
	m.size = sz;
	push_queue_msg(queue, &m);
}

/// 从 harbor_msg_queue 队列头弹出 harbor_msg
static struct harbor_msg *
pop_queue(struct harbor_msg_queue * queue) {
	if (queue->head == queue->tail) {
		return NULL;
	}
	struct harbor_msg * slot = &queue->data[queue->head];
	queue->head = (queue->head + 1) % queue->size;
	return slot;
}

/// 创建 harbor_msg_queue 对象
static struct harbor_msg_queue *
new_queue() {
	struct harbor_msg_queue * queue = skynet_malloc(sizeof(*queue));
	queue->size = DEFAULT_QUEUE_SIZE;
	queue->head = 0;
	queue->tail = 0;
	queue->data = skynet_malloc(DEFAULT_QUEUE_SIZE * sizeof(struct harbor_msg));

	return queue;
}

/// 释放 harbor_msg_queue 的资源, 包括所管理的 harbor_msg 的 buffer 指向的数据资源
static void
release_queue(struct harbor_msg_queue *queue) {
	if (queue == NULL)
		return;

	struct harbor_msg * m;
	while ((m=pop_queue(queue)) != NULL) {
		skynet_free(m->buffer);
	}

	skynet_free(queue->data);
	skynet_free(queue);
}

/// 从 hashmap 中搜索 key 与 name 相同的 keyvalue. 搜索到返回 keyvalue, 否则返回 NULL.
static struct keyvalue *
hash_search(struct hashmap * hash, const char name[GLOBALNAME_LENGTH]) {
	uint32_t *ptr = (uint32_t*) name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];	// 计算 hash 键
	struct keyvalue * node = hash->node[h % HASH_SIZE];
	while (node) {
		if (node->hash == h && strncmp(node->key, name, GLOBALNAME_LENGTH) == 0) {
			return node;
		}
		node = node->next;
	}
	return NULL;
}

/*

// Don't support erase name yet
// 还不支持擦除名字

static struct void
hash_erase(struct hashmap * hash, char name[GLOBALNAME_LENGTH) {
	uint32_t *ptr = name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
	struct keyvalue ** ptr = &hash->node[h % HASH_SIZE];
	while (*ptr) {
		struct keyvalue * node = *ptr;
		if (node->hash == h && strncmp(node->key, name, GLOBALNAME_LENGTH) == 0) {
			_release_queue(node->queue);
			*ptr->next = node->next;
			skynet_free(node);
			return;
		}
		*ptr = &(node->next);
	}
}
*/

/// 向 hashmap 中插入 name, 生成新的 keyvalue, keyvalue 的 key 为 name. 返回 keyvalue.
static struct keyvalue *
hash_insert(struct hashmap * hash, const char name[GLOBALNAME_LENGTH]) {
	uint32_t *ptr = (uint32_t *)name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];		// 计算 hash 键

	// 获得首 keyvalue 指针
	struct keyvalue ** pkv = &hash->node[h % HASH_SIZE];

	// 分配 keyvalue 内存
	struct keyvalue * node = skynet_malloc(sizeof(*node));

	// 初始化 keyvalue
	memcpy(node->key, name, GLOBALNAME_LENGTH);
	node->next = *pkv;
	node->queue = NULL;
	node->hash = h;
	node->value = 0;
	*pkv = node;

	return node;
}

/// 创建 hashmap 对象
static struct hashmap * 
hash_new() {
	struct hashmap * h = skynet_malloc(sizeof(struct hashmap));
	memset(h,0,sizeof(*h));
	return h;
}

/// 删除 hashmap 及其关联的所有内存资源
static void
hash_delete(struct hashmap *hash) {
	int i;
	for (i = 0; i < HASH_SIZE; i++) {
		struct keyvalue * node = hash->node[i];
		while (node) {
			struct keyvalue * next = node->next;
			release_queue(node->queue);
			skynet_free(node);
			node = next;
		}
	}
	skynet_free(hash);
}

///////////////

/// 关闭 id 对应的 slave 的连接
static void
close_harbor(struct harbor *h, int id) {
	struct slave *s = &h->s[id];
	s->status = STATUS_DOWN;

	// 关闭与该 slave 的连接
	if (s->fd) {
		skynet_socket_close(h->ctx, s->fd);
	}

	// 释放 harbor_msg_queue 资源
	if (s->queue) {
		release_queue(s->queue);
		s->queue = NULL;
	}
}

/// 报告给 slave skynet_context 有 slave id 断开连接
static void
report_harbor_down(struct harbor *h, int id) {
	char down[64];
	int n = sprintf(down, "D %d", id);

	skynet_send(h->ctx, 0, h->slave, PTYPE_TEXT, 0, down, n);
}

/// 创建 struct harbor 对象
struct harbor *
harbor_create(void) {
	struct harbor * h = skynet_malloc(sizeof(*h));
	memset(h, 0, sizeof(*h));

	// 创建 hashmap
	h->map = hash_new();
	return h;
}

/// 释放 struct harbor 对象
void
harbor_release(struct harbor *h) {
	// 关闭与 slave 的连接
	int i;
	for (i=1;i<REMOTE_MAX;i++) {
		struct slave *s = &h->s[i];
		if (s->fd && s->status != STATUS_DOWN) {
			close_harbor(h,i);
			// don't call report_harbor_down.
			// never call skynet_send during module exit, because of dead lock
			// 不要调用 report_harbor_down 函数.
			// 在模块退出的时, 永远不要调用 skynet_send, 因为会死锁.
		}
	}

	// 释放 hashmap
	hash_delete(h->map);
	skynet_free(h);
}

/// 高位低字节存储数字数据
static inline void
to_bigendian(uint8_t *buffer, uint32_t n) {
	buffer[0] = (n >> 24) & 0xff;
	buffer[1] = (n >> 16) & 0xff;
	buffer[2] = (n >> 8) & 0xff;
	buffer[3] = n & 0xff;
}

/// 将 remote_message_header 数据存储到 message 中. 每 4 个字节存储对应的数据.
static inline void
header_to_message(const struct remote_message_header * header, uint8_t * message) {
	to_bigendian(message , header->source);
	to_bigendian(message+4 , header->destination);
	to_bigendian(message+8 , header->session);
}

/// 低字节存储高位转化为实际数字
static inline uint32_t
from_bigendian(uint32_t n) {
	union {
		uint32_t big;
		uint8_t bytes[4];
	} u;
	u.big = n;
	return u.bytes[0] << 24 | u.bytes[1] << 16 | u.bytes[2] << 8 | u.bytes[3];
}

/// 从 message 中读出 remote_message_header 数据
static inline void
message_to_header(const uint32_t *message, struct remote_message_header *header) {
	header->source = from_bigendian(message[0]);
	header->destination = from_bigendian(message[1]);
	header->session = from_bigendian(message[2]);
}

// socket package

/// 解析 msg, 并且将实际内容转发给本地的目的服务
static void
forward_local_messsage(struct harbor *h, void *msg, int sz) {
	const char * cookie = msg;

	// 指针移到最后 HEADER_COOKIE_LENGTH 的位置处
	cookie += sz - HEADER_COOKIE_LENGTH;

	// 读取 remote_message_header 的数据
	struct remote_message_header header;
	message_to_header((const uint32_t *)cookie, &header);

	uint32_t destination = header.destination;

	// 获得消息类型, 高 8 位存储消息类型
	int type = (destination >> HANDLE_REMOTE_SHIFT) | PTYPE_TAG_DONTCOPY;

	// 高 8 位存储节点 id
	destination = (destination & HANDLE_MASK) | ((uint32_t)h->id << HANDLE_REMOTE_SHIFT);

	// 给 destination 发送消息
	if (skynet_send(h->ctx, header.source, destination, type, (int)header.session, (void *)msg, sz-HEADER_COOKIE_LENGTH) < 0) {
		// 如果发送失败, 给 source 发送错误信息
		skynet_send(h->ctx, destination, header.source , PTYPE_ERROR, (int)header.session, NULL, 0);
		skynet_error(h->ctx, "Unknown destination :%x from :%x", destination, header.source);
	}
}

/**
 * 通过 socket 发送数据, 注意: 这个函数对 buffer 的数据进行了复制.
 * 节点间的数据格式为: 数据头(表示内容数据长度, 4 个字节) + 实际数据内容 + remote_message_header
 * @param ctx skynet_context
 * @param fd socket id
 * @param buffer 数据指针
 * @param sz 数据长度
 * @param cookie 消息头
 */
static void
send_remote(struct skynet_context * ctx, int fd, const char * buffer, size_t sz, struct remote_message_header * cookie) {
	// 加上 remote_message_header 的数据长度
	size_t sz_header = sz + sizeof(*cookie);
	if (sz_header > UINT32_MAX) {
		skynet_error(ctx, "remote message from :%08x to :%08x is too large.", cookie->source, cookie->destination);
		return;
	}

	// 分配数据空间
	uint8_t * sendbuf = skynet_malloc(sz_header + 4);

	// 前 4 个字节存储数据长度
	to_bigendian(sendbuf, (uint32_t)sz_header);

	// 拷贝数据内容
	memcpy(sendbuf + 4, buffer, sz);

	// 存储 remote_message_header 内容
	header_to_message(cookie, sendbuf + 4 + sz);

	// ignore send error, because if the connection is broken, the mainloop will recv a message.
	// 忽略发送错误, 因为如果连接断开, mainloop 将接收 1 消息.
	skynet_socket_send(ctx, fd, sendbuf, sz_header + 4);
}

/// 如果 slave 还未连接成功, 那么先将 keyvalue.queue 的 harbor_msg 全部传给 slave.queue; 否则将 keyvalue.queue 里面的 msg 全部发送 slave 主机.
static void
dispatch_name_queue(struct harbor *h, struct keyvalue * node) {
	struct harbor_msg_queue * queue = node->queue;

	// 获得 handle
	uint32_t handle = node->value;

	// 获取 harbor id
	int harbor_id = handle >> HANDLE_REMOTE_SHIFT;

	// 确保是有效的节点
	assert(harbor_id != 0);

	// skynet_context
	struct skynet_context * context = h->ctx;

	// slave
	struct slave *s = &h->s[harbor_id];

	// socket id
	int fd = s->fd;

	if (fd == 0) {	// 如果还未连接成功
		if (s->status == STATUS_DOWN) {	// 未启用的状态, 报告错误
			char tmp [GLOBALNAME_LENGTH + 1];
			memcpy(tmp, node->key, GLOBALNAME_LENGTH);
			tmp[GLOBALNAME_LENGTH] = '\0';
			skynet_error(context, "Drop message to %s (in harbor %d)", tmp, harbor_id);
		} else {	// 将 keyvalue.queue 里面的数据传给 slave.queue
			if (s->queue == NULL) {	// s->queue = node->queue
				s->queue = node->queue;
				node->queue = NULL;
			} else {	// 将 node->queue 里面的 harbor_msg 弹出, 再压入 s->queue
				struct harbor_msg * m;
				while ((m = pop_queue(queue)) != NULL) {
					push_queue_msg(s->queue, m);
				}
			}
		}

		return;
	}

	struct harbor_msg * m;
	while ((m = pop_queue(queue)) != NULL) {
		// 记录 skynet_context handle
		m->header.destination |= (handle & HANDLE_MASK);

		// 发送数据给远程主机
		send_remote(context, fd, m->buffer, m->size, &m->header);

		// 可以释放 buffer 内存资源, 因为 send_remote 函数对 buffer 的数据进行了复制
		skynet_free(m->buffer);
	}
}

/// 将 slave.queue 保存的 msg 全部发送给 slave 主机.
static void
dispatch_queue(struct harbor *h, int id) {
	// slave
	struct slave *s = &h->s[id];

	// socket id
	int fd = s->fd;
	assert(fd != 0);

	// harbor_msg_queue
	struct harbor_msg_queue *queue = s->queue;
	if (queue == NULL)
		return;

	// 将 harbor_msg_queue 里面的数据全部发送出去
	struct harbor_msg * m;
	while ((m = pop_queue(queue)) != NULL) {
		send_remote(h->ctx, fd, m->buffer, m->size, &m->header);
		skynet_free(m->buffer);
	}

	// 释放 harbor_msg_queue 资源
	release_queue(queue);
	s->queue = NULL;
}

/// 接收 socket 数据, 并且将接收的完整数据转发给目的服务
static void
push_socket_data(struct harbor *h, const struct skynet_socket_message * message) {
	// 只处理 SKYNET_SOCKET_TYPE_DATA 消息
	assert(message->type == SKYNET_SOCKET_TYPE_DATA);

	int fd = message->id;
	int i;
	int id = 0;
	struct slave * s = NULL;

	// 根据 socket id 查询到 slave
	for (i=1;i<REMOTE_MAX;i++) {
		if (h->s[i].fd == fd) {
			s = &h->s[i];
			id = i;	// slave id
			break;
		}
	}

	if (s == NULL) {
		skynet_free(message->buffer);
		skynet_error(h->ctx, "Invalid socket fd (%d) data", fd);
		return;
	}

	// buffer & sz
	uint8_t * buffer = (uint8_t *)message->buffer;
	int size = message->ud;

	for (;;) {
		switch(s->status) {
		case STATUS_HANDSHAKE: {
			// check id
			// 验证 id
			uint8_t remote_id = buffer[0];
			if (remote_id != id) {
				skynet_error(h->ctx, "Invalid shakehand id (%d) from fd = %d , harbor = %d", id, fd, remote_id);
				close_harbor(h,id);
				return;
			}

			++buffer;	// 读取数据起始指针
			--size;		// 剩余可读数据数量
			s->status = STATUS_HEADER;	// 读取数据头

			// 发送 slave.queue 里面的数据
			dispatch_queue(h, id);

			if (size == 0) {
				break;
			}
			// go though
		}

		case STATUS_HEADER: {
			// big endian 4 bytes length, the first one must be 0.
			// big endian 4 字节长度, 第一个字节必须为 0.
			int need = 4 - s->read;
			if (size < need) {	// 读取所能读取的数据
				memcpy(s->size + s->read, buffer, size);
				s->read += size;
				return;
			} else {	// 将数据头数据完全读取完
				memcpy(s->size + s->read, buffer, need);
				buffer += need;		// 剩余数据起始指针
				size -= need;		// 剩余读取数据长度

				// 数据长度校验
				if (s->size[0] != 0) {
					skynet_error(h->ctx, "Message is too long from harbor %d", id);
					close_harbor(h, id);
					return;
				}

				// 内容数据长度
				s->length = s->size[1] << 16 | s->size[2] << 8 | s->size[3];

				// 重置已读长度
				s->read = 0;

				// 分配内存
				s->recv_buffer = skynet_malloc(s->length);

				// 转到读取内容状态
				s->status = STATUS_CONTENT;

				// 缓存中的数据读取完
				if (size == 0) {
					return;
				}
			}
		}
		// go though
		case STATUS_CONTENT: {
			int need = s->length - s->read;
			if (size < need) {	// 读取所能读取的数据
				memcpy(s->recv_buffer + s->read, buffer, size);
				s->read += size;
				return;
			}

			memcpy(s->recv_buffer + s->read, buffer, need);

			// 将数据发送给其他服务
			forward_local_messsage(h, s->recv_buffer, s->length);

			// 重置数据, 下次需要读取数据头
			s->length = 0;
			s->read = 0;
			s->recv_buffer = NULL;

			size -= need;	// 剩余读取数据长度
			buffer += need;	// 剩余数据起始指针
			s->status = STATUS_HEADER;	// 再次读取数据头

			// 缓存中的数据读取完
			if (size == 0)
				return;
			break;
		}
		default:
			return;
		}
	}
}

/**
 * 更新/注册 slave 的名字. 
 * @param h harbor
 * @param name 新名字
 * @param handle skynet_context handle
 */
static void
update_name(struct harbor *h, const char name[GLOBALNAME_LENGTH], uint32_t handle) {
	// 查询 name 对应的 keyvalue
	struct keyvalue * node = hash_search(h->map, name);

	// 如果没有则插入 name, 生成新的 keyvalue
	if (node == NULL) {
		node = hash_insert(h->map, name);
	}

	// 记录 handle
	node->value = handle;

	// 将 keyvalue.queue 的数据发送出去
	if (node->queue) {
		dispatch_name_queue(h, node);
		release_queue(node->queue);
		node->queue = NULL;
	}
}

/// 发送数据给 destination. 返回值: 发送成功给 slave, 或者 slave 不存在返回 0, 否则返回 1.
static int
remote_send_handle(struct harbor *h, uint32_t source, uint32_t destination, int type, int session, const char * msg, size_t sz) {
	// 获得 harbor id, 注意这时高 8 位存储的还是 harbor id
	int harbor_id = destination >> HANDLE_REMOTE_SHIFT;

	struct skynet_context * context = h->ctx;

	// 本地消息就直接发送给本地服务
	if (harbor_id == h->id) {
		// local message
		// 本地消息
		skynet_send(context, source, destination, type | PTYPE_TAG_DONTCOPY, session, (void *)msg, sz);
		return 1;
	}

	// slave
	struct slave * s = &h->s[harbor_id];

	if (s->fd == 0 || s->status == STATUS_HANDSHAKE) {
		if (s->status == STATUS_DOWN) {
			// throw an error return to source
			// report the destination is dead
			// 抛出 1 个错误返回给 source, destination 死掉了
			skynet_send(context, destination, source, PTYPE_ERROR, 0 , NULL, 0);
			skynet_error(context, "Drop message to harbor %d from %x to %x (session = %d, msgsz = %d)", harbor_id, source, destination, session, (int)sz);
		} else {
			// 创建 harbor_msg_queue
			if (s->queue == NULL) {
				s->queue = new_queue();
			}

			// 压入 slave.queue 队列
			struct remote_message_header header;
			header.source = source;
			header.destination = (type << HANDLE_REMOTE_SHIFT) | (destination & HANDLE_MASK);
			header.session = (uint32_t)session;
			push_queue(s->queue, (void *)msg, sz, &header);
			return 1;
		}
	} else {
		// 直接将消息发给 slave 主机
		struct remote_message_header cookie;
		cookie.source = source;
		cookie.destination = (destination & HANDLE_MASK) | ((uint32_t)type << HANDLE_REMOTE_SHIFT);
		cookie.session = (uint32_t)session;
		send_remote(context, s->fd, msg, sz, &cookie);
	}

	return 0;
}

/// 发送数据给 name 对应的服务, 如果当前没有记录 name, 那么先发送查询请求给 .cslave 服务
static int
remote_send_name(struct harbor *h, uint32_t source, const char name[GLOBALNAME_LENGTH], int type, int session, const char * msg, size_t sz) {
	// 查询 keyvalue
	struct keyvalue * node = hash_search(h->map, name);

	// 没有 keyvalue 则注册添加新的 name
	if (node == NULL) {
		node = hash_insert(h->map, name);
	}

	if (node->value == 0) {	// 如果还不知道该全局名字对应的 handle, 那么先查询
		// 创建 harbor_msg_queue
		if (node->queue == NULL) {
			node->queue = new_queue();
		}

		// 初始化 remote_message_header
		struct remote_message_header header;
		header.source = source;
		header.destination = type << HANDLE_REMOTE_SHIFT;
		header.session = (uint32_t)session;

		// 压入 node->queue 队列
		push_queue(node->queue, (void *)msg, sz, &header);

		// 拼接字符串
		char query[2+GLOBALNAME_LENGTH+1] = "Q ";
		query[2+GLOBALNAME_LENGTH] = 0;
		memcpy(query+2, name, GLOBALNAME_LENGTH);

		// 给 slave skynet_context 发送信息
		skynet_send(h->ctx, 0, h->slave, PTYPE_TEXT, 0, query, strlen(query));
		return 1;
	} else {	// 如果已经知道对应的 handle, 直接发送消息
		return remote_send_handle(h, source, node->value, type, session, msg, sz);
	}
}

/// 发送'握手'请求
static void
handshake(struct harbor *h, int id) {
	struct slave *s = &h->s[id];
	uint8_t * handshake = skynet_malloc(1);
	handshake[0] = (uint8_t)h->id;
	skynet_socket_send(h->ctx, s->fd, handshake, 1);
}

/**
 * harbor 的命令处理
 * @param h harbor
 * @param msg 数据指针
 * @param sz 数据长度
 * @param session
 * @param source 数据发送源
 */
static void
harbor_command(struct harbor * h, const char * msg, size_t sz, int session, uint32_t source) {
	const char * name = msg + 2;	// 指向全局名字的指针
	int s = (int)sz;
	s -= 2;

	switch(msg[0]) {
	case 'N' : {	// 更新全局名字
		// 长度校验
		if (s <= 0 || s >= GLOBALNAME_LENGTH) {
			skynet_error(h->ctx, "Invalid global name %s", name);
			return;
		}

		// 将 name 和 handle 赋值给 struct remote_name
		struct remote_name rn;
		memset(&rn, 0, sizeof(rn));
		memcpy(rn.name, name, s);
		rn.handle = source;

		// 更新当前节点的 slave 信息
		update_name(h, rn.name, rn.handle);
		break;
	}
	case 'S' :		// 连接到新的 harbor(我连别人), 我们首先应该发送自己的 id 到 fd, 然后接收 1 个 id(检查它), 然后在最后发送队列.
	case 'A' : {	// 接收新的 harbor(别人连我), 我们应该发送自己的 id 到 fd, 然后发送队列.
		// 复制 name
		char buffer[s + 1];
		memcpy(buffer, name, s);
		buffer[s] = 0;

		// 获得 fd id
		int fd = 0, id = 0;
		sscanf(buffer, "%d %d", &fd, &id);

		// 参数验证
		if (fd == 0 || id <= 0 || id >= REMOTE_MAX) {
			skynet_error(h->ctx, "Invalid command %c %s", msg[0], buffer);
			return;
		}

		// 判断 slave 是否已经存在
		struct slave * slave = &h->s[id];
		if (slave->fd != 0) {
			skynet_error(h->ctx, "Harbor %d alreay exist", id);
			return;
		}

		// 记录与当前节点连接的 slave 的 fd
		slave->fd = fd;

		// 为当前 skynet_context 关联 socket
		skynet_socket_start(h->ctx, fd);

		// 发送握手数据
		// 'S' 时是请求
		// 'A' 时是响应
		handshake(h, id);

		if (msg[0] == 'S') {
			// 如果是我连其他客户端, 进入'握手'状态, 等待其他客户端的'握手'响应
			slave->status = STATUS_HANDSHAKE;
		} else {
			// 进入接收数据头状态
			slave->status = STATUS_HEADER;

			// 将之前的消息发送出去
			dispatch_queue(h, id);
		}
		break;
	}
	default:
		// 报错
		skynet_error(h->ctx, "Unknown command %s", msg);
		return;
	}
}

/// 根据 socket id 查询 slave id
static int
harbor_id(struct harbor *h, int fd) {
	int i;
	for (i=1;i<REMOTE_MAX;i++) {
		struct slave *s = &h->s[i];
		if (s->fd == fd) {
			return i;
		}
	}
	return 0;
}

/// skynet_context 的处理消息的回调函数, 主要逻辑处理. 返回 0, 需要删除 msg 资源.
static int
mainloop(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct harbor * h = ud;
	switch (type) {
	case PTYPE_SOCKET: {
		const struct skynet_socket_message * message = msg;
		switch(message->type) {
		case SKYNET_SOCKET_TYPE_DATA:
			// 接收的数据发送给 skynet_context slave
			push_socket_data(h, message);

			// message 由 skynet_context 删除, buffer 当前删除
			skynet_free(message->buffer);
			break;
		case SKYNET_SOCKET_TYPE_ERROR:
		case SKYNET_SOCKET_TYPE_CLOSE: {
			// 搜索到对应的 slave
			int id = harbor_id(h, message->id);
			if (id) {
				// 发送关闭通知
				report_harbor_down(h,id);
			} else {
				skynet_error(context, "Unkown fd (%d) closed", message->id);
			}
			break;
		}
		case SKYNET_SOCKET_TYPE_CONNECT:
			// fd forward to this service
			break;
		case SKYNET_SOCKET_TYPE_WARNING: {
			int id = harbor_id(h, message->id);
			if (id) {
				skynet_error(context, "message havn't send to Harbor (%d) reach %d K", id, message->ud);
			}
			break;
		}
		default:
			skynet_error(context, "recv invalid socket message type %d", type);
			break;
		}
		return 0;
	}

	case PTYPE_HARBOR: {
		harbor_command(h, msg,sz,session,source);
		return 0;
	}

	default: {
		// remote message out
		const struct remote_message *rmsg = msg;
		if (rmsg->destination.handle == 0) {
			if (remote_send_name(h, source , rmsg->destination.name, type, session, rmsg->message, rmsg->sz)) {
				return 0;
			}
		} else {
			if (remote_send_handle(h, source , rmsg->destination.handle, type, session, rmsg->message, rmsg->sz)) {
				return 0;
			}
		}

		// msg 由 skynet 删除, 
		skynet_free((void *)rmsg->message);
		return 0;
	}
	}
}

/// 初始化 struct harbor
int
harbor_init(struct harbor *h, struct skynet_context *ctx, const char * args) {
	h->ctx = ctx;
	int harbor_id = 0;
	uint32_t slave = 0;
	sscanf(args, "%d %u", &harbor_id, &slave);
	if (slave == 0) {
		return 1;
	}
	h->id = harbor_id;	// harbor id
	h->slave = slave;	// skynet_context handle
	skynet_callback(ctx, h, mainloop);
	skynet_harbor_start(ctx);

	return 0;
}
