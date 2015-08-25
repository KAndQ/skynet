#include "skynet.h"
#include "lua-seri.h"

// 这个是帮助在终端打印的时候, 对于错误信息以红色打印输出
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/// 错误处理函数
static int
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

/// 普通回调, 返回 0, 函数执行完毕之后删除 msg
static int
_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	lua_State *L = ud;
	int trace = 1;
	int r;
	int top = lua_gettop(L);
	if (top == 0) {	// 初始化, 压入需要的值
		lua_pushcfunction(L, traceback);
		lua_rawgetp(L, LUA_REGISTRYINDEX, _cb);
		// 这时栈结构, _cb对应的值, 是个函数(-1), traceback 函数(-2)
	} else {		// 每次运行的时候, 必须保证当前线程(Lua 的线程)只有上面初始化的值
		assert(top == 2);	// 保证 lua 栈上就 2 个元素, _cb 对应的函数对象(2), traceback 函数(1)
	}
	lua_pushvalue(L,2);

	lua_pushinteger(L, type);
	lua_pushlightuserdata(L, (void *)msg);
	lua_pushinteger(L, sz);
	lua_pushinteger(L, session);
	lua_pushinteger(L, source);

	r = lua_pcall(L, 5, 0 , trace);

	if (r == LUA_OK) {
		return 0;
	}
	const char * self = skynet_command(context, "REG", NULL);
	switch (r) {
	case LUA_ERRRUN:
		skynet_error(context, "lua call [%x to %s : %d msgsz = %d] error : " KRED "%s" KNRM, source , self, session, sz, lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		skynet_error(context, "lua memory error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRERR:
		skynet_error(context, "lua error in error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRGCMM:
		skynet_error(context, "lua gc error : [%x to %s : %d]", source , self, session);
		break;
	};

	// 无论是否发生错误, 都弹出 traceback 压入的字符串.
	lua_pop(L,1);

	return 0;
}

/// 消息转发模式回调, 返回 1, 不删除 msg
static int
forward_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	_cb(context, ud, type, session, source, msg, sz);
	// don't delete msg in forward mode.
	// 在消息转发模式中不要删除 msg. 可以参考 skynet_server.c 的 dispatch_message 方法, 返回 1 表示不删除 msg, 返回 0 需要删除 msg.
	return 1;
}

/**
 * 设置 skynet_context 的回调函数
 * lua: 接收 2 个参数, 参数 1, 回调的执行函数; 参数 2, 是否消息转发模式, 
 */
static int
_callback(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));

	int forward = lua_toboolean(L, 2);	// 是否消息转发模式

	luaL_checktype(L,1,LUA_TFUNCTION);
	lua_settop(L,1);	// 只保留第一个参数, 是个函数对象
	lua_rawsetp(L, LUA_REGISTRYINDEX, _cb);	// 注册表添加 _cb 键关联栈顶的函数值

	// LUA_RIDX_MAINTHREAD: 注册表中这个索引下是状态机的主线程。（主线程和状态机同时被创建出来。）
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
	lua_State *gL = lua_tothread(L,-1);	// 拿到当前的主线程

	if (forward) {
		skynet_callback(context, gL, forward_cb);
	} else {
		skynet_callback(context, gL, _cb);
	}

	return 0;
}

/**
 * 请求调用 C 的 skynet_commond 方法, 命令的执行参数和返回值都是字符串类型.
 * lua: 2 个或者 1 个参数, 参数 1, 命令; 参数 2, 提供给命令的参数, 字符串类型. 0 or 1 个返回值, 视命令而定, 如果有返回值, 那么返回值是字符串类型.
 */
static int
_command(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * result;
	const char * parm = NULL;
	if (lua_gettop(L) == 2) {
		parm = luaL_checkstring(L,2);
	}

	result = skynet_command(context, cmd, parm);
	if (result) {
		lua_pushstring(L, result);
		return 1;
	}
	return 0;
}

/**
 * 请求调用 C 的 skynet_commond 方法, 命令的执行参数和返回值都是整型.
 * lua: 2 个或者 1 个参数, 参数 1, 命令; 参数 2, 提供给命令的参数, 整型. 0 or 1 个返回值, 视命令而定, 如果有返回值, 那么返回值是整型
 */
static int
_intcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * result;
	const char * parm = NULL;
	char tmp[64];	// for integer parm
	if (lua_gettop(L) == 2) {
		int32_t n = (int32_t)luaL_checkinteger(L,2);
		sprintf(tmp, "%d", n);
		parm = tmp;
	}

	result = skynet_command(context, cmd, parm);
	if (result) {
		lua_Integer r = strtoll(result, NULL, 0);
		lua_pushinteger(L, r);
		return 1;
	}
	return 0;
}

/**
 * 请求分配一个新的 session
 * lua: 接收 0 个参数; 1 个返回值, 新的 session.
 */
static int
_genid(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int session = skynet_send(context, 0, 0, PTYPE_TAG_ALLOCSESSION , 0 , NULL, 0);
	lua_pushinteger(L, session);
	return 1;
}

/// 检验栈 index 处的值是否是字符串类型
static const char *
get_dest_string(lua_State *L, int index) {
	const char * dest_string = lua_tostring(L, index);
	if (dest_string == NULL) {
		luaL_error(L, "dest address type (%s) must be a string or number.", lua_typename(L, lua_type(L,index)));
	}
	return dest_string;
}

/*
	uint32 address
	string address
	integer type
	integer session
	string message
	lightuserdata message_ptr
	integer len

	----------------------------------------
	接收参数:
	address 服务地址, 可以是整型, 表示服务的 handle; 也可以是字符串, 表示服务的名字;
	type 类型, PTYPE_*
	session session, 如果传入的是 nil, 系统将自动分配 1 个 session
	message 消息数据, 可以是字符串类型; 也可以是 lightuserdata 类型, 不过对于 lightuserdata 类型, 必须还要再增加 1 个参数, 表示 lightuserdata 数据的大小

	返回值: 
	如果没有返回值, 表示发送可能发生了错误, 例如无效的地址. 否则拥有 1 个返回值, 表示这次发送的 session
 */
static int
_send(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));

	// 拿到第一个参数, dest or dest_string
	uint32_t dest = (uint32_t)lua_tointeger(L, 1);
	const char * dest_string = NULL;
	if (dest == 0) {
		if (lua_type(L,1) == LUA_TNUMBER) {
			return luaL_error(L, "Invalid service address 0");
		}
		dest_string = get_dest_string(L, 1);
	}

	// 拿到第二个参数, type
	int type = luaL_checkinteger(L, 2);

	// 第三个参数, session
	int session = 0;
	if (lua_isnil(L,3)) {
		type |= PTYPE_TAG_ALLOCSESSION;
	} else {
		session = luaL_checkinteger(L,3);
	}

	// 第四个参数, string or lightuserdata/size
	int mtype = lua_type(L,4);
	switch (mtype) {
	case LUA_TSTRING: {
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,4,&len);
		if (len == 0) {
			msg = NULL;
		}
		if (dest_string) {
			session = skynet_sendname(context, 0, dest_string, type, session , msg, len);
		} else {
			session = skynet_send(context, 0, dest, type, session , msg, len);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,4);
		int size = luaL_checkinteger(L,5);
		if (dest_string) {
			session = skynet_sendname(context, 0, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {
			session = skynet_send(context, 0, dest, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		}
		break;
	}
	default:
		luaL_error(L, "skynet.send invalid param %s", lua_typename(L, lua_type(L,4)));
	}

	if (session < 0) {
		// send to invalid address
		// todo: maybe throw an error would be better
		// 发送到无效的地址
		// 待办事项: 可能抛出一个错误将会更好一些.
		return 0;
	}

	// 压入返回结果
	lua_pushinteger(L,session);
	return 1;
}

/**
 * 它可以指定发送地址（把消息源伪装成另一个服务），指定发送的消息的 session 。注：source 都必须是数字地址，不可以是别名。
 * lua:
 * 	dest or dest_string
 * 	source
 * 	type
 * 	session
 * 	string or lightuserdata, sz
 * 无返回值.
 */
static int
_redirect(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));

	// 参数 1, dest or dest_string
	uint32_t dest = (uint32_t)lua_tointeger(L,1);
	const char * dest_string = NULL;
	if (dest == 0) {
		dest_string = get_dest_string(L, 1);
	}

	// 参数 2, source
	uint32_t source = (uint32_t)luaL_checkinteger(L,2);

	// 参数 3, type
	int type = luaL_checkinteger(L,3);

	// 参数 4, session
	int session = luaL_checkinteger(L,4);

	int mtype = lua_type(L,5);
	switch (mtype) {
	case LUA_TSTRING: {
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,5,&len);
		if (len == 0) {
			msg = NULL;
		}
		if (dest_string) {
			session = skynet_sendname(context, source, dest_string, type, session , msg, len);
		} else {
			session = skynet_send(context, source, dest, type, session , msg, len);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,5);
		int size = luaL_checkinteger(L,6);
		if (dest_string) {
			session = skynet_sendname(context, source, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {
			session = skynet_send(context, source, dest, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		}
		break;
	}
	default:
		luaL_error(L, "skynet.redirect invalid param %s", lua_typename(L,mtype));
	}
	return 0;
}

/**
 * 请求使用 skynet_error 发送错误消息
 * lua: 接收 1 个参数, 字符串; 无返回值.
 */
static int
_error(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	skynet_error(context, "%s", luaL_checkstring(L,1));
	return 0;
}

/**
 * 将这个 C 指针和长度翻译成 lua 的 string
 * lua: 接收 2 个参数, 参数 1, lightuserdata, 参数 2, lightuserdata 长度; 1 个返回值, 字符串.
 */
static int
_tostring(lua_State *L) {
	if (lua_isnoneornil(L,1)) {
		return 0;
	}
	char * msg = lua_touserdata(L,1);
	int sz = luaL_checkinteger(L,2);
	lua_pushlstring(L,msg,sz);
	return 1;
}

/**
 * 获得服务所属的节点。
 * lua: 接收 1 个参数, 服务的 handle; 2 个返回值, 节点的 harbor 和是否是远程节点的 boolean 类型
 */
static int
_harbor(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	uint32_t handle = (uint32_t)luaL_checkinteger(L,1);
	int harbor = 0;
	int remote = skynet_isremote(context, handle, &harbor);
	lua_pushinteger(L, harbor);
	lua_pushboolean(L, remote);

	return 2;
}

/**
 * 将 lua 对象序列化成 lua string 存储.
 * lua: 接收任意个参数; 1 个返回值, lua string, 注意这个 string 是序列化的二进制数据.
 */
static int
lpackstring(lua_State *L) {
	_luaseri_pack(L);
	char * str = (char *)lua_touserdata(L, -2);
	int sz = lua_tointeger(L, -1);
	lua_pushlstring(L, str, sz);
	skynet_free(str);
	return 1;
}

/**
 * 释放 lightuserdata 指向的内存
 * lua: 接收 2 个参数, 参数 1, lightuserdata, 参数 2, size; 无返回值.
 */
static int
ltrash(lua_State *L) {
	int t = lua_type(L,1);
	switch (t) {
	case LUA_TSTRING: {
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,1);
		luaL_checkinteger(L,2);
		skynet_free(msg);
		break;
	}
	default:
		luaL_error(L, "skynet.trash invalid param %s", lua_typename(L,t));
	}

	return 0;
}

int
luaopen_skynet_core(lua_State *L) {
	luaL_checkversion(L);

	luaL_Reg l[] = {
		{ "send" , _send },
		{ "genid", _genid },
		{ "redirect", _redirect },
		{ "command" , _command },
		{ "intcommand", _intcommand },
		{ "error", _error },
		{ "tostring", _tostring },
		{ "harbor", _harbor },
		{ "pack", _luaseri_pack },
		{ "unpack", _luaseri_unpack },
		{ "packstring", lpackstring },
		{ "trash" , ltrash },
		{ "callback", _callback },
		{ NULL, NULL },
	};

	// 创建一张新的表，并预分配足够保存下数组 l 内容的空间（但不填充）。 这是给 luaL_setfuncs 一起用的
	luaL_newlibtable(L, l);

	// 将所有函数关联 upvalue(skynet_context)
	// 将注册表的 "skynet_context" 的值压入到栈顶, 在 service_snlua.c 的 _init 函数中在注册表中添加了 "skynet_context" 这个域的值.
	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");
	struct skynet_context *ctx = lua_touserdata(L,-1);
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");
	}
	luaL_setfuncs(L,l,1);

	return 1;
}
