#include "skynet_malloc.h"

#include "skynet_socket.h"

#include <lua.h>
#include <lauxlib.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QUEUESIZE 1024		// 默认队列大小
#define HASHSIZE 4096		// uncomplete hash 存储表大小
#define SMALLSTRING 2048	// 目前没有使用

#define TYPE_DATA 1
#define TYPE_MORE 2
#define TYPE_ERROR 3
#define TYPE_OPEN 4
#define TYPE_CLOSE 5
#define TYPE_WARNING 6

/*
	Each package is uint16 + data , uint16 (serialized in big-endian) is the number of bytes comprising the data .
	每个数据包格式是 uint16 + data, uint16(以 big-endian 序列化) 表示的是包含数据的大小.
 */

/// 存储完整的数据包内容
struct netpack {
	int id;		// socket id
	int size;	// 数据大小
	void * buffer;	// 指向的数据内容
};

/// 还未完整读取的数据包内容
struct uncomplete {
	struct netpack pack;	// 正在读取的数据
	struct uncomplete * next;	// hash 表数据结构, 关联可能 hashid 相同的对象
	int read;		// 如果是正数, 表示已经读取的数据内容; 如果是负数(-1), 表示只读取了包头的 1 个字节
	int header;		// 当 read == -1 时, 存储能够读取到 1 个字节包头数据
};

struct queue {
	int cap;		// 队列总大小
	int head;		// 队列头
	int tail;		// 队列尾, 指向还未使用的元素
	struct uncomplete * hash[HASHSIZE];	// 栈数据存储/hash 查询, 还未完整读取的数据包
	struct netpack queue[QUEUESIZE];	// netpack 队列数据结构, 已经完整读取的数据包
};

/// 释放 uncomplete 链表数据
static void
clear_list(struct uncomplete * uc) {
	while (uc) {
		void * tmp = uc;
		uc = uc->next;
		skynet_free(tmp);
	}
}

// 释放 struct queue
// lua: 接受 1 个参数, userdata 类型, 表示 struct queue; 0 个返回值.
static int
lclear(lua_State *L) {
	struct queue * q = lua_touserdata(L, 1);
	if (q == NULL) {
		return 0;
	}

	// 释放 struct uncomplete
	int i;
	for (i = 0; i < HASHSIZE; i++) {
		clear_list(q->hash[i]);
		q->hash[i] = NULL;
	}

	// 方便后面的运算, 直接取余就好了
	if (q->head > q->tail) {
		q->tail += q->cap;
	}

	// 释放 netpack.buffer 指向的资源
	for (i=q->head;i<q->tail;i++) {
		struct netpack *np = &q->queue[i % q->cap];
		skynet_free(np->buffer);
	}
	q->head = q->tail = 0;

	return 0;
}

/// 对 fd 做一个 hash 运算, 返回 hash 值
static inline int
hash_fd(int fd) {
	int a = fd >> 24;
	int b = fd >> 12;
	int c = fd;
	return (int)(((uint32_t)(a + b + c)) % HASHSIZE);
}

/// 从 queue 中查询到符合的 uncomplete, 同时将查询到的 uncomplete 从 queue.hash 中删除
static struct uncomplete *
find_uncomplete(struct queue *q, int fd) {
	if (q == NULL)
		return NULL;

	int h = hash_fd(fd);

	struct uncomplete * uc = q->hash[h];
	if (uc == NULL)
		return NULL;

	// 返回查询到的元素
	if (uc->pack.id == fd) {
		q->hash[h] = uc->next;
		return uc;
	}

	struct uncomplete * last = uc;
	while (last->next) {
		uc = last->next;
		if (uc->pack.id == fd) {
			last->next = uc->next;
			return uc;
		}
		last = uc;
	}
	return NULL;
}

/// 获取或者生成 queue, 确保 queue 在 lua 栈的 1 位置. 返回 queue.
static struct queue *
get_queue(lua_State *L) {
	struct queue *q = lua_touserdata(L,1);
	if (q == NULL) {
		q = lua_newuserdata(L, sizeof(struct queue));
		q->cap = QUEUESIZE;
		q->head = 0;
		q->tail = 0;
		int i;
		for (i = 0; i < HASHSIZE; i++) {
			q->hash[i] = NULL;
		}
		lua_replace(L, 1);
	}
	return q;
}

/// 扩展 queue 的 netpack queue 的大小
static void
expand_queue(lua_State *L, struct queue *q) {
	struct queue *nq = lua_newuserdata(L, sizeof(struct queue) + q->cap * sizeof(struct netpack));
	nq->cap = q->cap + QUEUESIZE;
	nq->head = 0;
	nq->tail = q->cap;

	// 复制 queue.hash 的内容
	memcpy(nq->hash, q->hash, sizeof(nq->hash));

	// 清空原来的 queue.hash
	memset(q->hash, 0, sizeof(q->hash));

	// 复制 queue.queue 的内容
	int i;
	for (i=0;i<q->cap;i++) {
		int idx = (q->head + i) % q->cap;
		nq->queue[i] = q->queue[idx];
	}

	// 设置原来的 queue 为空
	q->head = q->tail = 0;
	lua_replace(L,1);
}

/// 将数据压入 queue.queue 中
static void
push_data(lua_State *L, int fd, void *buffer, int size, int clone) {
	// 如果需要复制, 则分配新的内存空间, 复制数据
	if (clone) {
		void * tmp = skynet_malloc(size);
		memcpy(tmp, buffer, size);
		buffer = tmp;
	}

	// 将数据压入到 queue.queue 中
	struct queue *q = get_queue(L);
	struct netpack *np = &q->queue[q->tail];
	if (++q->tail >= q->cap)
		q->tail -= q->cap;
	np->id = fd;
	np->buffer = buffer;
	np->size = size;

	// 扩展队列空间
	if (q->head == q->tail) {
		expand_queue(L, q);
	}
}

/// 生成新的 uncomplete 对象, 压入到 queue.hash 中, 存储 fd. 返回生成的 uncomplete 对象指针.
static struct uncomplete *
save_uncomplete(lua_State *L, int fd) {
	struct queue *q = get_queue(L);
	int h = hash_fd(fd);
	struct uncomplete * uc = skynet_malloc(sizeof(struct uncomplete));
	memset(uc, 0, sizeof(*uc));
	uc->next = q->hash[h];
	uc->pack.id = fd;
	q->hash[h] = uc;

	return uc;
}

/// 从 buffer 中读取数据, 只是从 buffer 的前两个字节读取. 返回读取的数据.
static inline int
read_size(uint8_t * buffer) {
	int r = (int)buffer[0] << 8 | (int)buffer[1];
	return r;
}

/// 从 buffer 中读取数据. 如果 buffer 的数据足够, 那么数据内容会压入到 queue.queue 中; 否则会压入到 queue.hash 中.
static void
push_more(lua_State *L, int fd, uint8_t *buffer, int size) {
	// 如果连包头的数据都不足够, 那么先读取包头的第一个字节数据
	if (size == 1) {
		struct uncomplete * uc = save_uncomplete(L, fd);
		uc->read = -1;
		uc->header = *buffer;
		return;
	}

	// 读取数据包数据大小
	int pack_size = read_size(buffer);

	// 偏移到实际数据内容指针
	buffer += 2;

	// 得到实际数据的内容大小
	size -= 2;

	// 数据内容未能完全读取的情况
	if (size < pack_size) {
		struct uncomplete * uc = save_uncomplete(L, fd);
		uc->read = size;			// 记录已经读取的数据大小
		uc->pack.size = pack_size;	// 记录数据内容的大小
		uc->pack.buffer = skynet_malloc(pack_size);
		memcpy(uc->pack.buffer, buffer, size);
		return;
	}

	// 如果数据内容能够完全读取, 那么读取数据, 并且压入到 queue.queue 中
	push_data(L, fd, buffer, pack_size, 1);

	// 继续读取剩余的函数
	buffer += pack_size;
	size -= pack_size;
	if (size > 0) {
		push_more(L, fd, buffer, size);
	}
}

/// 释放掉 fd 对应的 uncomplete
static void
close_uncomplete(lua_State *L, int fd) {
	struct queue *q = lua_touserdata(L, 1);
	struct uncomplete * uc = find_uncomplete(q, fd);
	if (uc) {
		skynet_free(uc->pack.buffer);
		skynet_free(uc);
	}
}

/// 从 buffer 读取数据, 返回 lua 函数返回值的个数, 函数的第一个返回值是 queue.
static int
filter_data_(lua_State *L, int fd, uint8_t * buffer, int size) {
	struct queue *q = lua_touserdata(L, 1);
	struct uncomplete * uc = find_uncomplete(q, fd);	// 注意, 这里将 uncomplete 从 queue.hash 中移除了
	if (uc) {
		// fill uncomplete
		// 填充 uncomplete
		if (uc->read < 0) {		// 当包头还未完整读取的情况
			// read size
			assert(uc->read == -1);

			// 获得数据内容大小
			int pack_size = *buffer;
			pack_size |= uc->header << 8 ;

			// 偏移到实际数据内容指针开始位置
			++buffer;

			// 实际的数据内容大小
			--size;

			uc->pack.size = pack_size;	// 记录实际需要读取的内容大小
			uc->pack.buffer = skynet_malloc(pack_size);	// 分配内存空间
			uc->read = 0;	// 标记还未开始读取数据内容
		}

		// 计算需要读取的数据
		int need = uc->pack.size - uc->read;

		if (size < need) {	// 如果 buffer 待读取的数据还不足, 尽可能读取能够读取的数据
			// 读取可读的数据
			memcpy(uc->pack.buffer + uc->read, buffer, size);
			uc->read += size;

			// 再次压入到 queue.hash 中
			int h = hash_fd(fd);
			uc->next = q->hash[h];
			q->hash[h] = uc;
			return 1;
		}

		// 读取完整的数据内容
		memcpy(uc->pack.buffer + uc->read, buffer, need);

		// 跳过已经读取的内容
		buffer += need;

		// 计算剩余的可读取数据大小
		size -= need;

		// buffer 中的数据恰好足够读取
		if (size == 0) {
			lua_pushvalue(L, lua_upvalueindex(TYPE_DATA));	// macro TYPE_DATA
			lua_pushinteger(L, fd);	// socket id
			lua_pushlightuserdata(L, uc->pack.buffer);	// buffer
			lua_pushinteger(L, uc->pack.size);	// buffer size
			skynet_free(uc);
			return 5;
		}

		// more data
		// buffer 有更多的数据可读, 将数据压入 queue.queue 中
		push_data(L, fd, uc->pack.buffer, uc->pack.size, 0);
		skynet_free(uc);
		push_more(L, fd, buffer, size);	// 继续读取剩下的数据
		lua_pushvalue(L, lua_upvalueindex(TYPE_MORE));	// macro TYPE_MORE
		return 2;
	} else {
		if (size == 1) {	// 仅读取包头的 1 个数据
			struct uncomplete * uc = save_uncomplete(L, fd);
			uc->read = -1;
			uc->header = *buffer;
			return 1;
		}

		// 读取包头的数据
		int pack_size = read_size(buffer);
		buffer+=2;
		size-=2;

		// 如果 buffer 的数据不够读, 将 buffer 数据全部读取
		if (size < pack_size) {
			struct uncomplete * uc = save_uncomplete(L, fd);
			uc->read = size;
			uc->pack.size = pack_size;
			uc->pack.buffer = skynet_malloc(pack_size);
			memcpy(uc->pack.buffer, buffer, size);
			return 1;
		}

		// 如果 buffer 的数据恰好是 1 个包的数据大小, 将 buffer 数据全部读取
		if (size == pack_size) {
			// just one package
			lua_pushvalue(L, lua_upvalueindex(TYPE_DATA));	// macro TYPE_DATA
			lua_pushinteger(L, fd);				// socket id
			void * result = skynet_malloc(pack_size);
			memcpy(result, buffer, size);
			lua_pushlightuserdata(L, result);	// buffer
			lua_pushinteger(L, size);			// buffer size
			return 5;
		}

		// more data
		// 如果 buffer 的数据大于 1 个包的数据大小, 那么继续读取 buffer 里面的数据
		push_data(L, fd, buffer, pack_size, 1);
		buffer += pack_size;
		size -= pack_size;
		push_more(L, fd, buffer, size);
		lua_pushvalue(L, lua_upvalueindex(TYPE_MORE));	// macro TYPE_MORE

		return 2;
	}
}

/// 从 buffer 读取数据, 会释放掉 buffer 指向的内存空间. 返回 lua 函数返回值的个数, 函数的第一个返回值是 queue.
static inline int
filter_data(lua_State *L, int fd, uint8_t * buffer, int size) {
	int ret = filter_data_(L, fd, buffer, size);
	// buffer is the data of socket message, it malloc at socket_server.c : function forward_message .
	// it should be free before return,
	// buffer 是 socket 消息数据, 它在 socket_server.c 的 forward_message 函数中被分配. 它应该在函数返回前被释放.
	skynet_free(buffer);
	return ret;
}

/// 压入 msg 字符串, 如果 msg == NULL, 那么压入空字符串
static void
pushstring(lua_State *L, const char * msg, int size) {
	if (msg) {
		lua_pushlstring(L, msg, size);
	} else {
		lua_pushliteral(L, "");
	}
}

/*
	userdata queue
	lightuserdata msg
	integer size
	return
		userdata queue
		integer type
		integer fd
		string msg | lightuserdata/integer

	接受 3 个参数
	userdata queue
	lightuserdata msg
	integer size

	返回参数的顺序
	userdata queue, 这个是一定会返回的
	integer type 视情况而定
	integer fd socket id
	string msg 或者 lightuserdata/integer

 */
static int
lfilter(lua_State *L) {
	// msg
	struct skynet_socket_message *message = lua_touserdata(L,2);

	// size
	int size = luaL_checkinteger(L,3);

	char * buffer = message->buffer;
	if (buffer == NULL) {
		buffer = (char *)(message + 1);
		size -= sizeof(*message);
	} else {
		size = -1;
	}

	lua_settop(L, 1); // 只保留 queue

	switch(message->type) {
	case SKYNET_SOCKET_TYPE_DATA:
		// ignore listen id (message->id)
		assert(size == -1);	// never padding string
		return filter_data(L, message->id, (uint8_t *)buffer, message->ud);
	case SKYNET_SOCKET_TYPE_CONNECT:
		// ignore listen fd connect
		return 1;
	case SKYNET_SOCKET_TYPE_CLOSE:
		// no more data in fd (message->id)
		close_uncomplete(L, message->id);
		lua_pushvalue(L, lua_upvalueindex(TYPE_CLOSE));
		lua_pushinteger(L, message->id);
		return 3;
	case SKYNET_SOCKET_TYPE_ACCEPT:
		lua_pushvalue(L, lua_upvalueindex(TYPE_OPEN));
		// ignore listen id (message->id);
		lua_pushinteger(L, message->ud);
		pushstring(L, buffer, size);
		return 4;
	case SKYNET_SOCKET_TYPE_ERROR:
		// no more data in fd (message->id)
		close_uncomplete(L, message->id);
		lua_pushvalue(L, lua_upvalueindex(TYPE_ERROR));
		lua_pushinteger(L, message->id);
		pushstring(L, buffer, size);
		return 4;
	case SKYNET_SOCKET_TYPE_WARNING:
		lua_pushvalue(L, lua_upvalueindex(TYPE_WARNING));
		lua_pushinteger(L, message->id);
		lua_pushinteger(L, message->ud);
		return 4;
	default:
		// never get here
		return 1;
	}
}

/*
	userdata queue
	return
		integer fd
		lightuserdata msg
		integer size

	接受 1 个参数, queue 对象
	返回 3 个参数
		integer fd socket id
		lightuserdata msg 数据内容
		integer size 数据内容大小
 */
/// 从 queue.queue 中弹出 1 个数据
static int
lpop(lua_State *L) {
	struct queue * q = lua_touserdata(L, 1);
	if (q == NULL || q->head == q->tail)
		return 0;

	// 从队列弹出数据
	struct netpack *np = &q->queue[q->head];
	if (++q->head >= q->cap) {
		q->head = 0;
	}

	lua_pushinteger(L, np->id);
	lua_pushlightuserdata(L, np->buffer);
	lua_pushinteger(L, np->size);

	return 3;
}

/*
	string msg | lightuserdata/integer

	lightuserdata/integer
 */

/**
 * 获得 lua 栈上 index 位置的对象的指针和数据大小
 * @param L lua_State
 * @param sz 数据大小
 * @param index 获取数据的索引
 * @return 指向数据的指针
 */
static const char *
tolstring(lua_State *L, size_t *sz, int index) {
	const char * ptr;
	if (lua_isuserdata(L,index)) {	// 如果栈 index 位置存储的是 userdata 类型对象, 那么获得 lightuserdata
		ptr = (const char *)lua_touserdata(L,index);
		*sz = (size_t)luaL_checkinteger(L, index + 1);
	} else {
		ptr = luaL_checklstring(L, index, sz);
	}
	return ptr;
}

// 使用 2 个字节存储 len 数值, len 的数值范围不应该超过 65535
static inline void
write_size(uint8_t * buffer, int len) {
	buffer[0] = (len >> 8) & 0xff;
	buffer[1] = len & 0xff;
}

// 将 string 对象或者 lightuserdata 重新打包, 生成新的 lightuserdata 和指向的数据大小, 新的 lightuserdata 相比原数据增加了 2 个字节表示数据大小
// lua: 接受 1 个参数, string 对象, 或者接受 2 个参数, lightuserdata 对象和指向数据大小; 返回 2 个参数, lightuserdata 和指向的数据大小
static int
lpack(lua_State *L) {
	size_t len;
	const char * ptr = tolstring(L, &len, 1);
	if (len > 0x10000) {
		return luaL_error(L, "Invalid size (too long) of data : %d", (int)len);
	}

	uint8_t * buffer = skynet_malloc(len + 2);
	write_size(buffer, len);
	memcpy(buffer+2, ptr, len);

	lua_pushlightuserdata(L, buffer);
	lua_pushinteger(L, len + 2);

	return 2;
}

// 将 lightuserdata 的数据转换成一个 string 数据
// lua: 接受 2 个参数, 第一个参数 lightuserdata, 第二个参数 lightuserdata 指向的数据大小; 返回 1 个参数, 转换的字符串对象.
static int
ltostring(lua_State *L) {
	// lightuserdata
	void * ptr = lua_touserdata(L, 1);

	// 数据大小
	int size = luaL_checkinteger(L, 2);

	if (ptr == NULL) {	// 若传入的为 nil, 那么返回空字符串
		lua_pushliteral(L, "");
	} else {
		// 使用字符串对象复制 lightuserdata 指向的数据
		lua_pushlstring(L, (const char *)ptr, size);
		skynet_free(ptr);	// 释放掉 lightuserdata 指向的数据内容
	}
	return 1;
}

int
luaopen_netpack(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "pop", lpop },
		{ "pack", lpack },
		{ "clear", lclear },
		{ "tostring", ltostring },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);

	// the order is same with macros : TYPE_* (defined top)
	// 与上面定义的宏顺序相同
	lua_pushliteral(L, "data");
	lua_pushliteral(L, "more");
	lua_pushliteral(L, "error");
	lua_pushliteral(L, "open");
	lua_pushliteral(L, "close");
	lua_pushliteral(L, "warning");

	lua_pushcclosure(L, lfilter, 6);
	lua_setfield(L, -2, "filter");

	return 1;
}
