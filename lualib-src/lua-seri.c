/*
	modify from https://github.com/cloudwu/lua-serialize
 */

#include "skynet_malloc.h"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define TYPE_NIL 0			// nil 类型
#define TYPE_BOOLEAN 1		// boolean 类型, hibits 0 false 1 true

#define TYPE_NUMBER 2		// 数字类型

// hibits 0 : 0 , 1: byte, 2:word, 4: dword, 6: qword, 8 : double
#define TYPE_NUMBER_ZERO 0	// 数字类型 0
#define TYPE_NUMBER_BYTE 1	// 数字类型 1 个字节
#define TYPE_NUMBER_WORD 2	// 数字类型 2 个字节
#define TYPE_NUMBER_DWORD 4	// 数字类型 4 个字节
#define TYPE_NUMBER_QWORD 6	// 数字类型 8 个字节
#define TYPE_NUMBER_REAL 8	// 数字类型 8 个字节, 浮点数据

#define TYPE_USERDATA 3
#define TYPE_SHORT_STRING 4

// hibits 0~31 : len
#define TYPE_LONG_STRING 5
#define TYPE_TABLE 6

// 限定小型数据存储的极限大小, 这个数字的出现其实也跟下面的 COMBINE_TYPE 宏有关系.
// 因为对于使用 1 个字节来存储数据长度, 低 3 位已经用来表示类型了, 那么省下的 6 位能够表示的最大数字是 31,
// 所以这个数字的作用其实是当希望使用 1 个字节来存储类型和长度数据的时候, 长度数据所能表示的最大值 + 1.
#define MAX_COOKIE 32

/// 组合类型和值, 低 3 位用来存储类型, 之后的高位用来存储 v
#define COMBINE_TYPE(t,v) ((t) | (v) << 3)

#define BLOCK_SIZE 128		// 数据块大小
#define MAX_DEPTH 32		// 允许 table 的类型的最大深度

/// 数据块节点
struct block {
	struct block * next;			// 下一个关联的节点
	char buffer[BLOCK_SIZE];		// 存储数据
};

/// 写数据块队列
struct write_block {
	struct block * head;		// 队列头
	struct block * current;		// 当前正在写入数据的 block
	int len;		// 写入数据的总大小
	int ptr;		// 记录 current block 写入数据的数量
};

/// 读数据 block
struct read_block {
	char * buffer;	// 装载读取数据的起始指针
	int len;		// 当前剩余的可读容量
	int ptr;		// 记录当前已读数据的数量
};

/// 分配一个 struct block 数据对象
inline static struct block *
blk_alloc(void) {
	struct block *b = skynet_malloc(sizeof(struct block));
	b->next = NULL;
	return b;
}

/**
 * 写入 sz 大小的数据到 write_block 中, 注意, 对 buf 的数据进行了复制.
 * @param b write_block
 * @param buf 写入数据的起始指针
 * @param sz 需要写入数据的大小
 */
inline static void
wb_push(struct write_block *b, const void *buf, int sz) {
	const char * buffer = buf;

	// 没有可用的 block, 分配新的 block
	if (b->ptr == BLOCK_SIZE) {
_again:
		b->current = b->current->next = blk_alloc();
		b->ptr = 0;
	}

	// block 数据足够使用
	if (b->ptr <= BLOCK_SIZE - sz) {
		memcpy(b->current->buffer + b->ptr, buffer, sz);
		b->ptr+=sz;
		b->len+=sz;

	// block 数据空间不足
	} else {
		int copy = BLOCK_SIZE - b->ptr;
		memcpy(b->current->buffer + b->ptr, buffer, copy);
		buffer += copy;
		b->len += copy;
		sz -= copy;
		goto _again;
	}
}

/// 初始化 write_block
static void
wb_init(struct write_block *wb , struct block *b) {
	wb->head = b;
	assert(b->next == NULL);
	wb->len = 0;
	wb->current = wb->head;
	wb->ptr = 0;
}

/// 释放 write_block 的 block 资源
static void
wb_free(struct write_block *wb) {
	struct block *blk = wb->head;
	blk = blk->next;	// the first block is on stack, 保留第一个 block 在栈上
	while (blk) {
		struct block * next = blk->next;
		skynet_free(blk);
		blk = next;
	}
	wb->head = NULL;
	wb->current = NULL;
	wb->ptr = 0;
	wb->len = 0;
}

/// read_block 初始化 
static void
rball_init(struct read_block * rb, char * buffer, int size) {
	rb->buffer = buffer;
	rb->len = size;
	rb->ptr = 0;
}

/// 记录 read_block 读取了 sz 大小的数据, 返回剩余可读数据的指针地址
static void *
rb_read(struct read_block *rb, int sz) {
	if (rb->len < sz) {
		return NULL;
	}

	int ptr = rb->ptr;
	rb->ptr += sz;
	rb->len -= sz;
	return rb->buffer + ptr;
}

/// write_block 压入 nil 数据
static inline void
wb_nil(struct write_block *wb) {
	uint8_t n = TYPE_NIL;
	wb_push(wb, &n, 1);
}

/// write_block 压入 boolean 数据
static inline void
wb_boolean(struct write_block *wb, int boolean) {
	uint8_t n = COMBINE_TYPE(TYPE_BOOLEAN , boolean ? 1 : 0);
	wb_push(wb, &n, 1);
}

/// write_block 压入 integer 数据
static inline void
wb_integer(struct write_block *wb, lua_Integer v) {
	int type = TYPE_NUMBER;
	if (v == 0) {
		// 存储 type 和 子类型, 因为是 ZERO 类型, 所以取值的时候只能是 0
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_ZERO);
		wb_push(wb, &n, 1);
	} else if (v != (int32_t)v) {
		// 存储 type 和 子类型
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_QWORD);
		wb_push(wb, &n, 1);

		// 存储值
		int64_t v64 = v;
		wb_push(wb, &v64, sizeof(v64));
	} else if (v < 0) {
		// 存储 type 和 子类型		
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);
		wb_push(wb, &n, 1);

		// 存储值
		int32_t v32 = (int32_t)v;
		wb_push(wb, &v32, sizeof(v32));
	} else if (v<0x100) {
		// 存储 type 和 子类型
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_BYTE);
		wb_push(wb, &n, 1);

		// 存储值
		uint8_t byte = (uint8_t)v;
		wb_push(wb, &byte, sizeof(byte));
	} else if (v<0x10000) {
		// 存储 type 和 子类型
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_WORD);
		wb_push(wb, &n, 1);

		// 存储值
		uint16_t word = (uint16_t)v;
		wb_push(wb, &word, sizeof(word));
	} else {
		// 存储 type 和 子类型
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);
		wb_push(wb, &n, 1);

		// 存储值
		uint32_t v32 = (uint32_t)v;
		wb_push(wb, &v32, sizeof(v32));
	}
}

/// write_block 压入浮点数据
static inline void
wb_real(struct write_block *wb, double v) {
	uint8_t n = COMBINE_TYPE(TYPE_NUMBER , TYPE_NUMBER_REAL);
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

/// write_block 压入 v 的地址值
static inline void
wb_pointer(struct write_block *wb, void *v) {
	uint8_t n = TYPE_USERDATA;
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

/// write_block 压入字符串数据
static inline void
wb_string(struct write_block *wb, const char *str, int len) {
	// 存储小字符串数据
	if (len < MAX_COOKIE) {
		uint8_t n = COMBINE_TYPE(TYPE_SHORT_STRING, len);
		wb_push(wb, &n, 1);
		if (len > 0) {
			wb_push(wb, str, len);
		}

	// 存储大字符串数据
	} else {
		uint8_t n;
		if (len < 0x10000) {	// 字符串长度 2 个字节之内
			n = COMBINE_TYPE(TYPE_LONG_STRING, 2);
			wb_push(wb, &n, 1);
			uint16_t x = (uint16_t) len;
			wb_push(wb, &x, 2);
		} else {				// 字符串长度 4 个字节之内
			n = COMBINE_TYPE(TYPE_LONG_STRING, 4);
			wb_push(wb, &n, 1);
			uint32_t x = (uint32_t) len;
			wb_push(wb, &x, 4);
		}
		wb_push(wb, str, len);
	}
}

static void pack_one(lua_State *L, struct write_block *b, int index, int depth);

/// write_block 压入数组的 table, 返回数组的 table 的长度
static int
wb_table_array(lua_State *L, struct write_block * wb, int index, int depth) {
	// 取长度操作（'#'）应得到的值
	int array_size = lua_rawlen(L,index);

	if (array_size >= MAX_COOKIE-1) {	// 大数组的存储方式
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, MAX_COOKIE-1);
		wb_push(wb, &n, 1);
		wb_integer(wb, array_size);
	} else {	// 小数组的存储方式
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, array_size);
		wb_push(wb, &n, 1);
	}

	int i;
	for (i=1;i<=array_size;i++) {
		// 将 table 的数据压入到栈顶
		lua_rawgeti(L,index,i);

		// 存储值, 注意, 数组类型这里只存储值, 而不存储键
		pack_one(L, wb, -1, depth);

		// 从栈顶弹出
		lua_pop(L,1);
	}

	return array_size;
}

/// write_block 压入非数组的 table
static void
wb_table_hash(lua_State *L, struct write_block * wb, int index, int depth, int array_size) {
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {	// 类似 next 函数, 键在 (-2), 值在 (-1)
		if (lua_type(L,-2) == LUA_TNUMBER) {	// 判断是数字
			if (lua_isinteger(L, -2)) {	// 判断是整型
				// 如果在是在数组范围内, 那么之前的 wb_table_array 已经把数据存储了, 进行下一次循环
				lua_Integer x = lua_tointeger(L,-2);
				if (x>0 && x<=array_size) {
					lua_pop(L,1);	// 弹出值, 因为键要作为 next 函数的参数
					continue;
				}
			}
		}

		// 存储 键
		pack_one(L,wb,-2,depth);

		// 存储 值
		pack_one(L,wb,-1,depth);

		// 弹出值, 因为键要作为 next 函数的参数
		lua_pop(L, 1);
	}

	// 当前的 table 已经存储完成
	wb_nil(wb);
}

/// write_block 压入带有原表, 并且原表有 __pairs 方法的 table
static void
wb_table_metapairs(lua_State *L, struct write_block *wb, int index, int depth) {

	// 下面注释中用到的 next 函数也可以理解为 __pairs 对应的函数, 它的传参形式和返回结果形式同 next 函数.

	// 记录当前是 table 类型
	uint8_t n = COMBINE_TYPE(TYPE_TABLE, 0);
	wb_push(wb, &n, 1);

	// 目前栈顶是个函数, 之后才是 table, 现在对这个 table 做一个副本, 压入到栈顶
	lua_pushvalue(L, index);

	// 执行函数, 这时函数的 3 个返回值压栈, 参考下面的 <<__pairs 介绍>>
	// 函数返回值将按正序压栈（第一个返回值首先压栈）， 因此在调用结束后，最后一个返回值将被放在栈顶。
	lua_call(L, 1, 3);

	// 这时栈结构: next函数(-1), index表(-2), (-3) ...
	// 这时栈结构: nil(-1), index表(-2), next函数(-3) ...

	for(;;) {
		// index 表位于栈顶
		lua_pushvalue(L, -2);

		// nil 位于栈顶, 或者表中的某个键
		lua_pushvalue(L, -2);

		// 这时栈结构: nil/表中某个键(-1), index表(-2), nil(-3), index表(-4), next函数(-5) ...

		// void lua_copy (lua_State *L, int fromidx, int toidx);
		// 从索引 fromidx 处复制一个值到一个有效索引 toidx 处，覆盖那里的原有值。 不会影响其它位置的值。
		lua_copy(L, -5, -3);

		// 这时栈结构: nil/表中某个键(-1), index表(-2), next函数(-3), index表(-4), next函数(-5) ...
		// 执行 next 函数

		lua_call(L, 2, 2);

		// 如果表有后续元素
		// 这时的栈结构: value(-1), key(-2), index表(-3), next函数(-4) ...

		// 如果是最后的元素
		// 这时的栈结构: nil(-1), nil(-2), index表(-3), next函数(-4) ...

		int type = lua_type(L, -2);
		if (type == LUA_TNIL) {
			lua_pop(L, 4);
			break;
		}

		// 存储 键
		pack_one(L, wb, -2, depth);

		// 存储 值
		pack_one(L, wb, -1, depth);

		// 弹出值, 下一次循环这个键需要作为 next 函数的一个参数
		lua_pop(L, 1);
	}

	// 最后压入一个 nil 值, 表示这个表的加载结束了
	wb_nil(wb);
}

/// write_block 压入 table 类型数据
static void
wb_table(lua_State *L, struct write_block *wb, int index, int depth) {
	luaL_checkstack(L, LUA_MINSTACK, NULL);
	if (index < 0) {
		index = lua_gettop(L) + index + 1;
	}

	// __pairs 介绍
	// 如果 t 有元方法 __pairs， 以 t 为参数调用它，并返回其返回的前三个值。
	// 否则，返回三个值：next 函数， 表 t，以及 nil。

	// int luaL_getmetafield (lua_State *L, int obj, const char *e);
	// 将索引 obj 处对象的元表中 e 域的值压栈。 如果该对象没有元表，或是该元表没有相关域， 此函数什么也不会压栈并返回 LUA_TNIL。
	// 如果返回值不是 LUA_TNIL, 那么这时栈顶是 __pairs 对应的值, 是个函数, 现在可以再去具体的查看 wb_table_metapairs 函数。

	if (luaL_getmetafield(L, index, "__pairs") != LUA_TNIL) {
		wb_table_metapairs(L, wb, index, depth);
	} else {
		// 首先压入数组数据
		int array_size = wb_table_array(L, wb, index, depth);

		// 接着再压入非数组数据
		wb_table_hash(L, wb, index, depth, array_size);
	}
}

/// 从当前栈的 index 出取出 lua 对象, 并将这个对象序列化存储到 write_block 中.
static void
pack_one(lua_State *L, struct write_block *b, int index, int depth) {

	// 深度检测, 主要用于 table 类型
	if (depth > MAX_DEPTH) {
		wb_free(b);
		luaL_error(L, "serialize can't pack too depth table");
	}

	// 根据 type 选择各自的方式序列化存储
	int type = lua_type(L,index);
	switch(type) {
	case LUA_TNIL:
		wb_nil(b);
		break;
	case LUA_TNUMBER: {
		if (lua_isinteger(L, index)) {
			lua_Integer x = lua_tointeger(L,index);
			wb_integer(b, x);
		} else {
			lua_Number n = lua_tonumber(L,index);
			wb_real(b,n);
		}
		break;
	}
	case LUA_TBOOLEAN: 
		wb_boolean(b, lua_toboolean(L,index));
		break;
	case LUA_TSTRING: {
		size_t sz = 0;
		const char *str = lua_tolstring(L,index,&sz);
		wb_string(b, str, (int)sz);
		break;
	}
	case LUA_TLIGHTUSERDATA:
		wb_pointer(b, lua_touserdata(L,index));
		break;
	case LUA_TTABLE: {
		if (index < 0) {
			index = lua_gettop(L) + index + 1;
		}
		wb_table(L, b, index, depth+1);
		break;
	}
	default:
		wb_free(b);
		luaL_error(L, "Unsupport type %s to serialize", lua_typename(L, type));
	}
}

/// 从 lua 栈中 from + 1 的位置开始将数据压入到 write_block 中
static void
pack_from(lua_State *L, struct write_block *b, int from) {
	int n = lua_gettop(L) - from;
	int i;
	for (i=1;i<=n;i++) {
		pack_one(L, b , from + i, 0);
	}
}

/// 打印错误读取信息
static inline void
invalid_stream_line(lua_State *L, struct read_block *rb, int line) {
	int len = rb->len;
	luaL_error(L, "Invalid serialize stream %d (line:%d)", len, line);
}

/// 打印错误读取信息
#define invalid_stream(L,rb) invalid_stream_line(L,rb,__LINE__)

/// 从 read_block 中读取整型数据, 返回读取的整型数据
static lua_Integer
get_integer(lua_State *L, struct read_block *rb, int cookie) {
	switch (cookie) {
	case TYPE_NUMBER_ZERO:
		return 0;
	case TYPE_NUMBER_BYTE: {
		uint8_t n;
		uint8_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		n = *pn;
		return n;
	}
	case TYPE_NUMBER_WORD: {
		uint16_t n;
		uint16_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_DWORD: {
		int32_t n;
		int32_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_QWORD: {
		int64_t n;
		int64_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	default:
		invalid_stream(L,rb);
		return 0;
	}
}

/// 从 read_block 中读取浮点型数据, 返回读取的浮点型数据
static double
get_real(lua_State *L, struct read_block *rb) {
	double n;
	double * pn = rb_read(rb,sizeof(n));
	if (pn == NULL)
		invalid_stream(L,rb);
	memcpy(&n, pn, sizeof(n));
	return n;
}

/// 从 rb 读取地址值
static void *
get_pointer(lua_State *L, struct read_block *rb) {
	void * userdata = 0;
	void ** v = (void **)rb_read(rb,sizeof(userdata));
	if (v == NULL) {
		invalid_stream(L,rb);
	}
	memcpy(&userdata, v, sizeof(userdata));
	return userdata;
}

/// 从 rb 读取字符串数据
static void
get_buffer(lua_State *L, struct read_block *rb, int len) {
	char * p = rb_read(rb,len);
	if (p == NULL) {
		invalid_stream(L,rb);
	}
	lua_pushlstring(L,p,len);
}

static void unpack_one(lua_State *L, struct read_block *rb);

/// 从 read_block 中读取 table 压入到当前栈的栈顶
static void
unpack_table(lua_State *L, struct read_block *rb, int array_size) {

	// 是大数组的情况下
	if (array_size == MAX_COOKIE-1) {
		// 确认数据类型, 并且获得数组的实际长度
		uint8_t type;
		uint8_t *t = rb_read(rb, sizeof(type));
		if (t==NULL) {
			invalid_stream(L,rb);
		}
		type = *t;
		int cookie = type >> 3;
		if ((type & 7) != TYPE_NUMBER || cookie == TYPE_NUMBER_REAL) {
			invalid_stream(L,rb);
		}
		array_size = get_integer(L,rb,cookie);
	}

	luaL_checkstack(L,LUA_MINSTACK,NULL);
	lua_createtable(L,array_size,0);

	// 将连续的数组数据放入到 table 中
	int i;
	for (i=1;i<=array_size;i++) {
		unpack_one(L,rb);
		lua_rawseti(L,-2,i);
	}

	// 将散列的数据存储到 table 中
	for (;;) {
		// 值压入栈
		unpack_one(L,rb);
		if (lua_isnil(L,-1)) {
			lua_pop(L,1);
			return;
		}

		// 键压入栈
		unpack_one(L,rb);

		// 存入到 table 中
		lua_rawset(L,-3);
	}
}

/// 从 read_block 读取出数据压入到 lua 当前栈的栈顶
static void
push_value(lua_State *L, struct read_block *rb, int type, int cookie) {
	switch(type) {
	case TYPE_NIL:
		lua_pushnil(L);
		break;
	case TYPE_BOOLEAN:
		lua_pushboolean(L,cookie);
		break;
	case TYPE_NUMBER:
		if (cookie == TYPE_NUMBER_REAL) {
			lua_pushnumber(L,get_real(L,rb));
		} else {
			lua_pushinteger(L, get_integer(L, rb, cookie));
		}
		break;
	case TYPE_USERDATA:
		lua_pushlightuserdata(L,get_pointer(L,rb));
		break;
	case TYPE_SHORT_STRING:
		get_buffer(L,rb,cookie);
		break;
	case TYPE_LONG_STRING: {
		if (cookie == 2) {
			uint16_t *plen = rb_read(rb, 2);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint16_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		} else {
			if (cookie != 4) {
				invalid_stream(L,rb);
			}
			uint32_t *plen = rb_read(rb, 4);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint32_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		}
		break;
	}
	case TYPE_TABLE: {
		unpack_table(L,rb,cookie);
		break;
	}
	default: {
		invalid_stream(L,rb);
		break;
	}
	}
}

/// 从 read_block 中将二进制数据反序列化回 lua 对象, 会将数据压入当前栈的栈顶
static void
unpack_one(lua_State *L, struct read_block *rb) {
	// 读取一个字节数据
	uint8_t type;
	uint8_t *t = rb_read(rb, sizeof(type));
	if (t==NULL) {
		invalid_stream(L, rb);
	}
	type = *t;

	push_value(L, rb, type & 0x7, type>>3);
}

/// 分配 len 大小的内存空间, 将连续的 block 数据读取出来, 最后将指向分配的内存空间的指针作为 lightuserdata 类型压入 lua, 然后再压入 len.
static void
seri(lua_State *L, struct block *b, int len) {
	uint8_t * buffer = skynet_malloc(len);
	uint8_t * ptr = buffer;
	int sz = len;
	while(len>0) {
		if (len >= BLOCK_SIZE) {
			memcpy(ptr, b->buffer, BLOCK_SIZE);
			ptr += BLOCK_SIZE;
			len -= BLOCK_SIZE;
			b = b->next;
		} else {
			memcpy(ptr, b->buffer, len);
			break;
		}
	}
	
	lua_pushlightuserdata(L, buffer);
	lua_pushinteger(L, sz);
}

int
_luaseri_unpack(lua_State *L) {
	// 保证有参数
	if (lua_isnoneornil(L,1)) {
		return 0;
	}

	// 拿到起始地址指针和数据长度
	void * buffer;
	int len;
	if (lua_type(L,1) == LUA_TSTRING) {
		size_t sz;
		buffer = (void *)lua_tolstring(L,1,&sz);
		len = (int)sz;
	} else {
		buffer = lua_touserdata(L,1);
		len = luaL_checkinteger(L,2);
	}

	if (len == 0) {
		return 0;
	}

	if (buffer == NULL) {
		return luaL_error(L, "deserialize null pointer");
	}

	// 栈上所有元素移除
	lua_settop(L,0);

	// 初始化 read_block
	struct read_block rb;
	rball_init(&rb, buffer, len);

	int i;
	for (i=0;;i++) {

		// 扩展栈空间
		if (i%8==7) {
			luaL_checkstack(L,LUA_MINSTACK,NULL);
		}

		uint8_t type = 0;
		uint8_t *t = rb_read(&rb, sizeof(type));

		// 数据读取完毕
		if (t==NULL)
			break;

		// 将值反序列化压入到 lua 栈中
		type = *t;
		push_value(L, &rb, type & 0x7, type>>3);
	}

	// Need not free buffer
	// 不必释放 buffer

	return lua_gettop(L);
}

int
_luaseri_pack(lua_State *L) {
	struct block temp;		// 注意, 第一个 block 是在栈上的, 所以 wb_free 里面有段注释 "the first block is on stack", 第一个 block 是不能被 free
	temp.next = NULL;

	struct write_block wb;
	wb_init(&wb, &temp);

	// 将全部传入的参数序列化
	pack_from(L,&wb,0);

	// 校验, 不能改变 write_block 的 head 指针
	assert(wb.head == &temp);

	// 将 lightuserdata 类型和 sz 压入 lua, 作为返回值
	seri(L, &temp, wb.len);

	wb_free(&wb);

	return 2;
}
