#include "skynet_malloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include <lua.h>
#include <lauxlib.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "skynet_socket.h"

#define BACKLOG 32	// 默认的 listen 的 backlog 参数
// 2 ** 12 == 4096
#define LARGE_PAGE_NODE 12		// 决定分配缓存数据块的最大数量, 2 的次方
#define BUFFER_LIMIT (256 * 1024)	// 暂时没有使用

/// 缓存节点
struct buffer_node {
	char * msg;	// 数据指针
	int sz;	// 数据大小
	struct buffer_node *next;	// 关联的下一个节点
};

/// socket 数据缓存, 队列数据结构, 这里保存的 buffer_node 都是引用可用数据的
struct socket_buffer {
	int size;		// 当前链表存储数据的总大小
	int offset;		// 当前正在读取的 buffer_node 的指针偏移量, 因为可能存在当前的 buffer_node 的数据只读取了部分的情况, 所以需要记录已经被读取的内容
	struct buffer_node *head;	// 指向队列头元素的指针
	struct buffer_node *tail;	// 指向队列尾元素的指针
};
 
/**
 * 函数作用: 释放一组 buffer_node 资源.
 * lua: 接收 1 个参数, 0 个返回值
 */
static int
lfreepool(lua_State *L) {
	// 获取第一个参数, userdata类型(struct buffer_node, 通过 lnewpool)分配
	struct buffer_node * pool = lua_touserdata(L, 1);

	// 返回给定索引处值的固有“长度”： 
	// 1. 对于字符串，它指字符串的长度； 
	// 2. 对于表；它指不触发元方法的情况下取长度操作（'#'）应得到的值；
	// 3. 对于用户数据，它指为该用户数据分配的内存块的大小；
	// 4. 对于其它值，它为 0。
	int sz = lua_rawlen(L, 1) / sizeof(*pool);

	int i;
	for (i = 0; i < sz; i++) {
		struct buffer_node *node = &pool[i];
		if (node->msg) {
			skynet_free(node->msg);
			node->msg = NULL;
		}
	}
	return 0;
}

/// 分配一组 buffer_node 资源
static int
lnewpool(lua_State *L, int sz) {

	// 分配内存资源, 栈顶元素是 userdata 类型
	struct buffer_node * pool = lua_newuserdata(L, sizeof(struct buffer_node) * sz);

	// 初始化数据内容
	int i;
	for (i=0;i<sz;i++) {
		pool[i].msg = NULL;
		pool[i].sz = 0;
		pool[i].next = &pool[i+1];
	}
	pool[sz-1].next = NULL;

	// luaL_newmetatable 这时栈顶是一个 table 类型
	if (luaL_newmetatable(L, "buffer_pool")) {
		// 给栈顶的表的 __gc 字段关联 lfreepool 函数, 给分配的 userdata 添加终结器, 执行释放内存前的一些操作.
		lua_pushcfunction(L, lfreepool);
		lua_setfield(L, -2, "__gc");
	}

	// 将栈顶的表, 设置为 userdata 的原表
	lua_setmetatable(L, -2);
	return 1;
}

/**
 * 创建一个 socket_buffer, 压入栈
 * lua: 接收 0 个参数, 1 个返回值
 */
static int
lnewbuffer(lua_State *L) {
	struct socket_buffer * sb = lua_newuserdata(L, sizeof(*sb));	
	sb->size = 0;
	sb->offset = 0;
	sb->head = NULL;
	sb->tail = NULL;
	
	return 1;
}

/*
	userdata send_buffer(socket_buffer)
	table pool
	lightuserdata msg
	int size

	return size

	Comment: The table pool record all the buffers chunk, 
	and the first index [1] is a lightuserdata : free_node. We can always use this pointer for struct buffer_node .
	The following ([2] ...)  userdatas in table pool is the buffer chunk (for struct buffer_node), 
	we never free them until the VM closed. The size of first chunk ([2]) is 8 struct buffer_node,
	and the second size is 16 ... The largest size of chunk is LARGE_PAGE_NODE (4096)

	lpushbbuffer will get a free struct buffer_node from table pool, and then put the msg/size in it.
	lpopbuffer return the struct buffer_node back to table pool (By calling return_free_node).
	------------------------------------------------------------------------------------

	接收 4 个参数
	userdata socket_buffer
	table pool
	lightuserdata msg
	int size

	1 个返回值, 返回 socket_buffer 的 size
	return size

	注释: pool 表记录所有的缓存数据块, pool 表的第一个索引 [1] 是一个 lightuserdata 类型: free_node. 我们可以一直将这个指针作为 struct buffer_node 指针使用.
	接下来的索引(从 [2] 开始)是 userdata 类型, 它是一个缓存数据块(适用于 struct buffer_node), 我们从来不会释放它们, 直到 VM 被关闭. 第一个数据块([2])的大小
	是 16 个 struct buffer_node, 接着的第二个大小是 32 个, 以此类推. 最大的数据块大小是 LARGE_PAGE_NODE(4096) 个.

	其实 pool[1] 是一个 buffer_node 链表(栈的数据结构), 得到的是当前可用的 buffer_node.
	push 函数是将 buffer_node 从 pool[1] 中弹出到 socket_buffer 中;
	pop 函数是将 buffer_node 从 socket_buffer 弹出, 同时释放 buffer_node.msg 的数据资源, 再添加到 pool[1] 中, 让这个 buffer_node 又可以再次使用.

	lpushbuffer 将从 pool 表中获得一个空闲的 struct buffer_node, 然后设置 msg/size 的值.
	lpopbuffer 返回一个 struct buffer_node 给 pool 表(通过调用 retune_free_node 函数).

	从 pool[1] 中拿出未使用的数据 buffer_node, 使用 buffer_node 存储传入的值, 将已经记录数据的 buffer_node 添加到 socket_buffer 队列中.
	简易流程: data = pool[1] ====>>>> pool[1] = data.next ====>>>> socket_buffer.push(data)
 */
static int
lpushbuffer(lua_State *L) {
	// 获取第 1 个参数, userdata: socket_buffer
	struct socket_buffer *sb = lua_touserdata(L,1);
	if (sb == NULL) {
		return luaL_error(L, "need buffer object at param 1");
	}

	// 获取第 3 个参数, lightuserdata msg
	char * msg = lua_touserdata(L,3);
	if (msg == NULL) {
		return luaL_error(L, "need message block at param 3");
	}

	// 获取第 2 个参数, table pool
	int pool_index = 2;
	luaL_checktype(L,pool_index,LUA_TTABLE);

	// 获取第 4 个参数, int size
	int sz = luaL_checkinteger(L,4);

	// 拿到 table pool 的第 1 个元素 free_node
	lua_rawgeti(L,pool_index,1);
	struct buffer_node * free_node = lua_touserdata(L,-1);	// sb poolt msg size free_node
	lua_pop(L,1);

	// 当没有可用的 buffer_node 时, 分配新的内存空间
	if (free_node == NULL) {
		int tsz = lua_rawlen(L,pool_index);
		if (tsz == 0)
			tsz++;

		// 决定分配
		int size = 8;
		if (tsz <= LARGE_PAGE_NODE-3) {
			size <<= tsz;		// 最小分配 16
		} else {
			size <<= LARGE_PAGE_NODE-3;	// 最大分配 4096
		}

		// 分配内存数据压入到栈顶
		lnewpool(L, size);
		free_node = lua_touserdata(L, -1);

		// pool[tsz + 1] = 栈顶 userdata(free_node), tsz 从 1 开始计数, 所以这里就像上面注释说的, 从 [2] 开始存储连续的数据块.
		lua_rawseti(L, pool_index, tsz+1);
	}

	// 将当前 free_node 的下一个节点作为 pool[1] 的元素
	lua_pushlightuserdata(L, free_node->next);
	lua_rawseti(L, pool_index, 1);	// sb poolt msg size

	// 当前的 free_node 记录数据
	free_node->msg = msg;
	free_node->sz = sz;
	free_node->next = NULL;

	// 将 free_node 加入到 socket_buffer
	if (sb->head == NULL) {
		assert(sb->tail == NULL);
		sb->head = sb->tail = free_node;
	} else {
		sb->tail->next = free_node;
		sb->tail = free_node;
	}
	sb->size += sz;

	// 压入 sb->size 作为返回值
	lua_pushinteger(L, sb->size);

	return 1;
}

/**
 * 从 socket_buffer 队列中弹出一个 buffer_node, buffer_node 的 next 指向当前的 pool[1], 然后再把 pool[1] 设置为当前的 buffer_node
 * 简易流程: data = socket_buffer.pop() ====>>>> data.next = pool[1] ====>>>> free(data.msg) ====>>>> pool[1] = data
 */
static void
return_free_node(lua_State *L, int pool, struct socket_buffer *sb) {
	// 拿到第一个 socket_buffer 的头元素, 并将头元素弹出队列
	struct buffer_node *free_node = sb->head;
	sb->offset = 0;
	sb->head = free_node->next;
	if (sb->head == NULL) {
		sb->tail = NULL;
	}

	// 获得 pool table 中的第一个元素(free_node)
	lua_rawgeti(L,pool,1);

	// free_node->next = pool[1]
	free_node->next = lua_touserdata(L,-1);
	lua_pop(L,1);

	// 释放 buffer_node.msg 内存资源
	skynet_free(free_node->msg);
	free_node->msg = NULL;
	free_node->sz = 0;

	// pool[1] = free_node
	lua_pushlightuserdata(L, free_node);
	lua_rawseti(L, pool, 1);
}

/**
 * 从 sb 中读取 sz 大小的数据压入到 lua 中, 压入到 lua 的数据大小是 (sz - skip). lua 栈顶将压入一个 string 类型, 是从 socket_buffer 读取的数据.
 * sz: 需要读取数据的大小
 * skip: 分隔符字符串的长度
 */
static void
pop_lstring(lua_State *L, struct socket_buffer *sb, int sz, int skip) {
	struct buffer_node * current = sb->head;

	// 已经完整的读取了数据
	if (sz < current->sz - sb->offset) {	// 待读取的数据容量大于需求量, 即: 读取完 sz 大小的数据之后, 当前 buffer_node 还有数据可以读取
		lua_pushlstring(L, current->msg + sb->offset, sz-skip/* 减掉分隔符的大小 */);	// 将数据压入 lua
		sb->offset+=sz;	// 偏移量记录
		return;
	}

	// 已经完整的读取了数据
	if (sz == current->sz - sb->offset) {	// 待读取数据容量恰好等于需求量
		lua_pushlstring(L, current->msg + sb->offset, sz-skip /* 减掉分隔符的大小 */);	// 将数据压入 lua
		return_free_node(L,2,sb);	// 当前的 buffer_node 数据已经读取完毕, 释放掉当前 buffer_node 数据
		return;
	}

	// 字符串缓存 的类型
	// 字符串缓存可以让 C 代码分段构造一个 Lua 字符串。 使用模式如下：
	// 首先定义一个类型为 luaL_Buffer 的变量 b。
	// 调用 luaL_buffinit(L, &b) 初始化它。
	// 然后调用 luaL_add* 这组函数向其添加字符串片断。
	// 最后调用 luaL_pushresult(&b) 。 最后这次调用会在栈顶留下最终的字符串。
	luaL_Buffer b;

	// 初始化缓存 B。 这个函数不会分配任何空间； 缓存必须以一个变量的形式声明
	luaL_buffinit(L, &b);
	for (;;) {
		// 得到当前 buffer_node 剩余可以读取的数据容量
		int bytes = current->sz - sb->offset;

		if (bytes >= sz) {	// 注意: 第一次循环是不会执行这里的, 因为 >= 的情况在上面的代码中已经做了判断, 第二次循环将会有可能进入下面的循环.
			// 读取剩余的字符串数据
			if (sz > skip) {
				luaL_addlstring(&b, current->msg + sb->offset, sz - skip);
			} 

			// 偏移量记录
			sb->offset += sz;

			// 如果数据恰好可以完整读取, 释放掉当前 buffer_node 数据
			if (bytes == sz) {
				return_free_node(L,2,sb);
			}
			break;
		}

		// 获得实际需要读取的数据容量大小
		int real_sz = sz - skip;

		// 将数据压入到 lua
		if (real_sz > 0) {
			luaL_addlstring(&b, current->msg + sb->offset, (real_sz < bytes) ? real_sz : bytes);
		}
		return_free_node(L,2,sb); // 当前的 buffer_node 数据已经读取完毕, 释放掉当前 buffer_node 数据

		// 减掉已经读取的数据内容
		sz-=bytes;

		// 数据已经全部读取完毕
		if (sz==0)
			break;

		// 获取新的 buffer_node
		current = sb->head;
		assert(current);
	}

	// 结束对缓存 B 的使用，将最终的字符串留在栈顶
	luaL_pushresult(&b);
}

/**
 * 目前看是获得一个数据头, 数据头的大小是 [1, 4]
 * lua: 接收 1 个参数 string 类型(严格来说, 读取的是 4 个字节的数据), 返回 1 个参数, integer 类型.
 */
static int
lheader(lua_State *L) {
	// 获得第一个参数, 4 个字节的数据
	size_t len;
	const uint8_t * s = (const uint8_t *)luaL_checklstring(L, 1, &len);
	if (len > 4 || len < 1) {
		return luaL_error(L, "Invalid read %s", s);
	}

	// 解析出数据, 将 len 数据组合成整型数据
	int i;
	size_t sz = 0;
	for (i=0;i<(int)len;i++) {
		sz <<= 8;
		sz |= s[i];
	}

	// lua 函数返回
	lua_pushinteger(L, (lua_Integer)sz);

	return 1;
}

/*
	userdata send_buffer
	table pool
	integer sz 
	
	-------------------------------------
	userdata socket_buffer
	table pool
	integer sz

	2 个返回值, 第一个返回值, 字符串数据, string 类型, 若 sz 参数不合适(没有足够的数据可读, 或者 sz = 0), 则压入 nil; 第二个返回值, socket_buffer 的剩余数据.

	从 socket_buffer 中获得 sz 大小的数据
 */
static int
lpopbuffer(lua_State *L) {
	// 获得第一个参数, socket_buffer
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		return luaL_error(L, "Need buffer object at param 1");
	}

	// 检测第二个参数, pool table
	luaL_checktype(L,2,LUA_TTABLE);

	// 获得第三个参数, size
	int sz = luaL_checkinteger(L,3);

	// 读取数据, 将数据压入到 lua 中, 作为第一个返回参数
	if (sb->size < sz || sz == 0) {
		lua_pushnil(L);
	} else {
		pop_lstring(L, sb, sz, 0);
		sb->size -= sz;
	}

	// 压入栈, 作为第二个返回参数
	lua_pushinteger(L, sb->size);

	return 2;
}

/*
	userdata send_buffer
	table pool

	-------------------------------------
	userdata socket_buffer
	table pool

	清除 socket_buffer 内的所有数据资源, 将所有弹出的空闲 buffer_node 放置到 pool[1] 栈中.
	lua: 接收 2 个参数, 0 个返回值.
 */
static int
lclearbuffer(lua_State *L) {
	// 获取第一个参数
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		return luaL_error(L, "Need buffer object at param 1");
	}

	// 检测第二个参数
	luaL_checktype(L,2,LUA_TTABLE);

	// 将 socket_buffer 的所有数据弹出, 并且释放其资源
	while(sb->head) {
		return_free_node(L,2,sb);
	}
	sb->size = 0;

	return 0;
}

/**
 * 读取 socket_buffer 中的所有数据
 * lua: 接收 2 个参数, 参数 1: socket_buffer, 参数 2: table pool; 1 个返回值, 所有 socket_buffer 中的数据.
 */
static int
lreadall(lua_State *L) {
	// 获得参数 1, socket_buffer
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		return luaL_error(L, "Need buffer object at param 1");
	}

	// 检查参数 2, table
	luaL_checktype(L,2,LUA_TTABLE);

	// 读取 socket_buffer 中的所有数据, 并且压入 lua
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	while(sb->head) {
		struct buffer_node *current = sb->head;
		luaL_addlstring(&b, current->msg + sb->offset, current->sz - sb->offset);
		return_free_node(L,2,sb);
	}
	luaL_pushresult(&b);
	sb->size = 0;

	return 1;
}

/**
 * 释放内存资源
 * lua: 接收 2 个参数, 参数 1: userdata, 释放内存资源的指针; 参数 2: 整型, 没有使用; 没有返回值.
 */
static int
ldrop(lua_State *L) {
	// 获得参数 1
	void * msg = lua_touserdata(L,1);

	// 检查参数 2
	luaL_checkinteger(L,2);

	skynet_free(msg);
	return 0;
}

/**
 * 分隔符检查
 * @param node buffer_node
 * @param from 指针偏移量, 从 buffer_node.msg + from 处开始比较
 * @param sep 分隔数据
 * @param seplen 分隔数据的长度
 * @return 如果 node 接下来的连续内存数据与 sep 相同, 返回 true, 否则返回 false.
 */
static bool
check_sep(struct buffer_node * node, int from, const char *sep, int seplen) {
	for (;;) {
		int sz = node->sz - from;

		// node 有足够的数据可读
		if (sz >= seplen) {
			// int memcmp(const void *buf1, const void *buf2, unsigned int count);
			// memcmp 是比较内存区域 buf1 和 buf2 的前 count 个字节。该函数是按字节比较的。
			// 当 buf1 < buf2 时，返回值 < 0; 当 buf1 == buf2 时，返回值 = 0; 当 buf1 > buf2 时，返回值 > 0
			return memcmp(node->msg+from,sep,seplen) == 0;
		}

		// node 没有足够的数据可读, 那么只比较可读的部分
		if (sz > 0) {
			if (memcmp(node->msg + from, sep, sz)) {
				return false;
			}
		}

		// 获得下一个节点
		node = node->next;

		// 跳过已经比较过的部分
		sep += sz;
		seplen -= sz;

		// 下个节点的偏移量总 0 开始
		from = 0;
	}
}

/*
	userdata send_buffer
	table pool , nil for check
	string sep

	----------------------------------------
	userdata socket_buffer
	table pool, nil 用于检查
	string sep: 分割字符串

	lua 返回值: 如果第二个参数为 nil, 同时如果分隔符有效(有期望的数据可读), 返回 true, 否则返回 nil(其实是无数据返回);
	如果第二个参数不为 nil, 同时分隔符有效, 那么返回读取的字符串, 否则返回 nil(其实也是无数据返回).
 */
static int
lreadline(lua_State *L) {
	// 获得参数 1, socket_buffer
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		return luaL_error(L, "Need buffer object at param 1");
	}

	// only check
	// 只检查
	bool check = !lua_istable(L, 2);

	// 获得第三个参数
	size_t seplen = 0;
	const char *sep = luaL_checklstring(L,3,&seplen);

	int i;
	struct buffer_node *current = sb->head;

	// 保证有数据可读
	if (current == NULL)
		return 0;

	int from = sb->offset;

	// 剩余可读数据
	int bytes = current->sz - from;

	for (i = 0; i <= sb->size - (int)seplen/* socket_buffer 中有足够的数据可读 */; i++) {
		if (check_sep(current, from, sep, seplen)) {
			if (check) {
				lua_pushboolean(L,true);
			} else {
				pop_lstring(L, sb, i+seplen, seplen);
				sb->size -= i+seplen;
			}
			return 1;
		}

		// 增加 1 个偏移量
		++from;

		// 减少可读数据数量
		--bytes;

		// 如果当前的 buffer_node 数据读完, 则继续从下一个节点读取
		if (bytes == 0) {
			// 重置数据
			current = current->next;
			from = 0;

			// 无数据可读则跳出循环
			if (current == NULL)
				break;
			bytes = current->sz;
		}
	}
	return 0;
}

/**
 * 将一个字符串转化为一个指针, string ====>>>> lightuserdata
 * lua: 接收 1 个参数, string 类型; 2 个返回值, 字符串的长度, lightuserdata.
 */
static int
lstr2p(lua_State *L) {
	// 获得参数 1, string
	size_t sz = 0;
	const char * str = luaL_checklstring(L,1,&sz);

	// 对字符串的内容进行复制
	void *ptr = skynet_malloc(sz);
	memcpy(ptr, str, sz);

	// 压入返回值
	lua_pushlightuserdata(L, ptr);
	lua_pushinteger(L, (int)sz);
	return 2;
}

// for skynet socket
// 用于 skynet socket

/*
	lightuserdata msg
	integer size

	return type n1 n2 ptr_or_string

	----------------------------------------
	接收 2 个参数
	lightuserdata msg, skynet_socket_message 指针
	integer size, 数据长度

	4 个返回值, type, n1, n2, 指针或者字符串
	type: skynet_socket_message.type
	n1: skynet_socket_message.id
	n2: skynet_socket_message.ud
	参数4: 如果 skynet_socket_message.buffer 有值, 则保存其指针; 否则保存 skynet_socket_message 后续内存的字符串数据
	参数5: 只用于 udp 协议, 存储的是地址信息
*/
static int
lunpack(lua_State *L) {
	// 获得参数 1, skynet_socket_message
	struct skynet_socket_message *message = lua_touserdata(L,1);

	// 获得参数 2, size
	int size = luaL_checkinteger(L,2);

	lua_pushinteger(L, message->type);	// 返回值 1
	lua_pushinteger(L, message->id);	// 返回值 2
	lua_pushinteger(L, message->ud);	// 返回值 3

	if (message->buffer == NULL) {	// 获取 skynet_socket_message 内存数据之后的内容, 可以查看 skynet_socket.c 的 forward_message 函数.
		lua_pushlstring(L, (char *)(message+1), size - sizeof(*message));
	} else {	// 保存数据的指针
		lua_pushlightuserdata(L, message->buffer);
	}

	// 获得 udp 的地址信息, 作为第 5 个参数
	if (message->type == SKYNET_SOCKET_TYPE_UDP) {
		int addrsz = 0;
		const char * addrstring = skynet_socket_udp_address(message, &addrsz);
		if (addrstring) {
			lua_pushlstring(L, addrstring, addrsz);
			return 5;
		}
	}

	return 4;
}

/**
 * 获得地址和端口
 * @param L lua_State
 * @param tmp 存储主机地址
 * @param addr 传入的地址信息字符串, 可能包含端口, 如果在没有传入端口参数的情况下, 会解析这个字符串得到端口数据
 * @param port_index 参数端口号在 lua 栈中的索引
 * @param port 存储端口号
 * @return 返回主机地址字符串, 不包含端口信息
 */
static const char *
address_port(lua_State *L, char *tmp, const char * addr, int port_index, int *port) {
	const char * host;
	if (lua_isnoneornil(L,port_index)/* 当给定索引无效或其值是 nil 时， 返回 1 ，否则返回 0 。 */) {

		// char *strchr(char* _Str,int _Ch)
		// 查找字符串s中首次出现字符c的位置
		// 返回首次出现 _Ch 的位置的指针，返回的地址是被查找字符串指针开始的第一个与 _Ch 相同字符的指针，如果 _Str 中不存在 _Ch 则返回 NULL。
		// 成功则返回要查找字符第一次出现的位置，失败返回 NULL

		host = strchr(addr, '[');
		if (host) {		// ipv6 字符串格式解析, 格式应该是 [xxxx:xxxx]:port
			// is ipv6
			++host;
			const char * sep = strchr(addr,']');
			if (sep == NULL) {
				luaL_error(L, "Invalid address %s.",addr);
			}
			memcpy(tmp, host, sep-host);
			tmp[sep-host] = '\0';
			host = tmp;
			sep = strchr(sep + 1, ':');
			if (sep == NULL) {
				luaL_error(L, "Invalid address %s.",addr);
			}
			*port = strtoul(sep+1,NULL,10);

		} else {		// ipv4 字符串格式解析, 格式应该是 xxx.xxx.xxx.xxx:port
			// is ipv4
			const char * sep = strchr(addr,':');
			if (sep == NULL) {
				luaL_error(L, "Invalid address %s.",addr);
			}
			memcpy(tmp, addr, sep-addr);
			tmp[sep-addr] = '\0';
			host = tmp;
			*port = strtoul(sep+1,NULL,10);
		}
	} else {
		host = addr;
		*port = luaL_optinteger(L,port_index, 0);
	}
	return host;
}

/**
 * 连接到指定的主机
 * lua: 接收 2 个参数, 参数 1, string 地址字符串; 参数 2, integer 端口号; 1 个返回值, 可供使用的 socket id
 */
static int
lconnect(lua_State *L) {

	// 获得第一个参数, 地址字符串
	size_t sz = 0;
	const char * addr = luaL_checklstring(L,1,&sz);

	// 获得第二个参数或数据, 端口号
	char tmp[sz];
	int port = 0;
	const char * host = address_port(L, tmp, addr, 2, &port);
	if (port == 0) {
		return luaL_error(L, "Invalid port");
	}

	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int id = skynet_socket_connect(ctx, host, port);
	lua_pushinteger(L, id);

	return 1;
}

/**
 * 请求关闭一个 socket
 * lua: 接收 1 个参数, 整型, socket id; 0 个返回值.
 */
static int
lclose(lua_State *L) {
	int id = luaL_checkinteger(L,1);
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	skynet_socket_close(ctx, id);
	return 0;
}

/**
 * 侦听指定的端口地址
 * lua: 接收 3 个参数, 参数 1, 主机地址; 参数 2, 端口; 参数 3, backlog, 如果不传此参数, 将使用默认值; 1 个返回值, socket id
 */
static int
llisten(lua_State *L) {
	const char * host = luaL_checkstring(L,1);
	int port = luaL_checkinteger(L,2);
	int backlog = luaL_optinteger(L,3,BACKLOG);
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int id = skynet_socket_listen(ctx, host,port,backlog);
	if (id < 0) {
		return luaL_error(L, "Listen error");
	}

	lua_pushinteger(L,id);
	return 1;
}

/**
 * 获得数据指针和数据长度
 * @param L lua_State
 * @param index 获得数据的 lua 栈索引
 * @param sz 获得数据的大小
 * @return 
 */
static void *
get_buffer(lua_State *L, int index, int *sz) {
	void *buffer;

	// userdata 类型
	if (lua_isuserdata(L,index)) {
		buffer = lua_touserdata(L,index);
		*sz = luaL_checkinteger(L,index+1);

	// string 类型
	} else {
		size_t len = 0;
		const char * str =  luaL_checklstring(L, index, &len);
		buffer = skynet_malloc(len);
		memcpy(buffer, str, len);
		*sz = (int)len;
	}
	return buffer;
}

/**
 * 使用高优先级队列发送数据
 * lua: 接收 2 或者 3 个参数, 参数 1, socket id; 参数 2, 如果是 string, 那么无需传入参数 3, 如果是 lightuserdata 那么需要参数 3, 表示数据的大小.
 * 1 个返回值, boolean 类型, true 表示成功.
 */
static int
lsend(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));

	// 获得参数 1
	int id = luaL_checkinteger(L, 1);

	// 获得参数 2, 3
	int sz = 0;
	void *buffer = get_buffer(L, 2, &sz);

	int err = skynet_socket_send(ctx, id, buffer, sz);
	lua_pushboolean(L, !err);
	return 1;
}

/**
 * 使用低优先级队列发送数据
 * lua: 接收 2 或者 3 个参数, 参数 1, socket id; 参数 2, 如果是 string, 那么无需传入参数 3, 如果是 lightuserdata 那么需要参数 3, 表示数据的大小.
 * 0 个返回值.
 */
static int
lsendlow(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));

	// 获得参数 1
	int id = luaL_checkinteger(L, 1);

	// 获得参数 2, 3
	int sz = 0;
	void *buffer = get_buffer(L, 2, &sz);

	skynet_socket_send_lowpriority(ctx, id, buffer, sz);
	return 0;
}

/**
 * bind 一个 socket fd
 * lua: 接收 1 个参数, socket fd; 1 个返回值, socket id
 */
static int
lbind(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int fd = luaL_checkinteger(L, 1);
	int id = skynet_socket_bind(ctx,fd);
	lua_pushinteger(L,id);
	return 1;
}

/**
 * 打开一个 socket
 * lua: 接收 1 个参数, socket id; 0 个返回值.
 */
static int
lstart(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int id = luaL_checkinteger(L, 1);
	skynet_socket_start(ctx,id);
	return 0;
}

/**
 * 设置 socket 的 nodelay, 禁用 nagle 算法
 * lua: 接收 1 个参数, socket id; 0 个返回值.
 */
static int
lnodelay(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int id = luaL_checkinteger(L, 1);
	skynet_socket_nodelay(ctx,id);
	return 0;
}

/**
 * 创建一个 udp socket
 * lua: 接收 2 个参数, 参数 1, string 地址字符串; 参数 2, integer 端口号; 1 个返回值, socket id.
 */
static int
ludp(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	size_t sz = 0;
	const char * addr = lua_tolstring(L,1,&sz);

	char tmp[sz];
	int port = 0;
	const char * host = NULL;
	if (addr) {
		host = address_port(L, tmp, addr, 2, &port);
	}

	int id = skynet_socket_udp(ctx, host, port);
	if (id < 0) {
		return luaL_error(L, "udp init failed");
	}
	lua_pushinteger(L, id);
	return 1;
}

/**
 * 设置指定 socket 的地址信息.
 * lua: 接收 3 个参数, 参数 1, socket id; 参数 2, string 地址字符串; 参数 3, integer 端口号; 0 个返回值.
 */
static int
ludp_connect(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int id = luaL_checkinteger(L, 1);

	size_t sz = 0;
	const char * addr = luaL_checklstring(L,2,&sz);

	char tmp[sz];
	int port = 0;
	const char * host = NULL;
	if (addr) {
		host = address_port(L, tmp, addr, 3, &port);
	}

	if (skynet_socket_udp_connect(ctx, id, host, port)) {
		return luaL_error(L, "udp connect failed");
	}

	return 0;
}

/**
 * 基于 udp 协议, 发送数据.
 * lua: 接收 3或者4个参数, 参数 1, socket id; 参数 2, 地址信息; 参数 3, 如果是 string, 那么无需传入参数 4, 如果是 lightuserdata 那么需要参数 4, 表示数据的大小.
 * 1 个返回值, boolean 类型, 操作成功返回 true
 */
static int
ludp_send(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int id = luaL_checkinteger(L, 1);

	const char * address = luaL_checkstring(L, 2);

	int sz = 0;
	void *buffer = get_buffer(L, 3, &sz);

	int err = skynet_socket_udp_send(ctx, id, address, buffer, sz);

	lua_pushboolean(L, !err);

	return 1;
}

/**
 * 基于 udp 协议, 获得地址信息
 * lua: 接收 1 个参数, 虽然是以 string 压入 lua, 但是实际是以二进制数据数据存储. 2 个返回值, 主机地址字符串, 端口.
 */
static int
ludp_address(lua_State *L) {
	// 获得参数 1
	size_t sz = 0;
	const uint8_t * addr = (const uint8_t *)luaL_checklstring(L, 1, &sz);

	// 拿到端口数据
	uint16_t port = 0;
	memcpy(&port, addr+1, sizeof(uint16_t));

	// 将一个16位数由网络字节顺序转换为主机字节顺序。
	port = ntohs(port);

	// 决定协议的类型, ipv4/ipv6
	const void * src = addr+3;
	char tmp[256];
	int family;
	if (sz == 1+2+4) {
		family = AF_INET;
	} else {
		if (sz != 1+2+16) {
			return luaL_error(L, "Invalid udp address");
		}
		family = AF_INET6;
	}

	// 拿到地址的字符串格式
	if (inet_ntop(family, src, tmp, sizeof(tmp)) == NULL) {
		return luaL_error(L, "Invalid udp address");
	}

	// 压入返回结果
	lua_pushstring(L, tmp);
	lua_pushinteger(L, port);
	return 2;
}

/// 注册 socket 模块到 lua 中
int
luaopen_socketdriver(lua_State *L) {
	// 检查调用它的内核是否是创建这个 Lua 状态机的内核。 以及调用它的代码是否使用了相同的 Lua 版本。 
	// 同时也检查调用它的内核与创建该 Lua 状态机的内核 是否使用了同一片地址空间。
	luaL_checkversion(L);

	luaL_Reg l[] = {
		{ "buffer", lnewbuffer },
		{ "push", lpushbuffer },
		{ "pop", lpopbuffer },
		{ "drop", ldrop },
		{ "readall", lreadall },
		{ "clear", lclearbuffer },
		{ "readline", lreadline },
		{ "str2p", lstr2p },
		{ "header", lheader },

		{ "unpack", lunpack },
		{ NULL, NULL },
	};

	// 创建一张新的表，并把列表 l 中的函数注册进去。
	luaL_newlib(L,l);	// 这时栈顶是 table

	luaL_Reg l2[] = {
		{ "connect", lconnect },
		{ "close", lclose },
		{ "listen", llisten },
		{ "send", lsend },
		{ "lsend", lsendlow },
		{ "bind", lbind },
		{ "start", lstart },
		{ "nodelay", lnodelay },
		{ "udp", ludp },
		{ "udp_connect", ludp_connect },
		{ "udp_send", ludp_send },
		{ "udp_address", ludp_address },
		{ NULL, NULL },
	};

	// 将注册表的 "skynet_context" 的值压入到栈顶, 在 service_snlua.c 的 _init 函数中在注册表中添加了 "skynet_context" 这个域的值.
	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");

	// 获得栈顶值
	struct skynet_context *ctx = lua_touserdata(L,-1);
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");
	}

	// void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup);
	// 把数组 l 中的所有函数注册到栈顶的表中(该表在可选的 upvalue 之下, 见下面的解说).
	// 若 nup 不为零， 所有的函数都共享 nup 个 upvalue。 这些值必须在调用之前，压在表之上。 这些值在注册完毕后都会从栈弹出。
	// 根据上面对 luaL_setfuncs 的描述, 当前共享的 upvalue 就是 userdata(skynet_context).
	luaL_setfuncs(L, l2, 1);

	return 1;
}
