#include "skynet.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct snlua {
	lua_State * L;
	struct skynet_context * ctx;
};

// LUA_CACHELIB may defined in patched lua for shared proto
// LUA_CACHELIB 可能已经在 lua 的共享原型补丁中定义
#ifdef LUA_CACHELIB

#define codecache luaopen_cache

#else

static int
cleardummy(lua_State *L) {
  return 0;
}

static int 
codecache(lua_State *L) {
	luaL_Reg l[] = {
		{ "clear", cleardummy },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);
	lua_getglobal(L, "loadfile");
	lua_setfield(L, -2, "loadfile");
	return 1;
}

#endif

static int 
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		// 从当前位置将回溯信息压栈
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

/// 报告错误给 launcher 服务
static void
_report_launcher_error(struct skynet_context *ctx) {
	// sizeof "ERROR" == 5
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);
}

/// 获取配置数据
static const char *
optstring(struct skynet_context *ctx, const char *key, const char * str) {
	const char * ret = skynet_command(ctx, "GETENV", key);
	if (ret == NULL) {
		return str;
	}
	return ret;
}

static int
_init(struct snlua *l, struct skynet_context *ctx, const char * args, size_t sz) {
	lua_State *L = l->L;
	l->ctx = ctx;

	// int lua_gc (lua_State *L, int what, int data);
	// 控制垃圾收集器。根据参数 what 发起不同的任务.
	lua_gc(L, LUA_GCSTOP, 0);	// 停止垃圾收集器

	// 注册表添加 LUA_NOENV = ture
	lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. 标记库忽略环境变量 */
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
	
	// 打开指定状态机中的所有 Lua 标准库。
	luaL_openlibs(L);

	// 注册表添加 skynet_context = ctx(lightuserdata)
	lua_pushlightuserdata(L, ctx);
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");

	// 添加 "skynet.codecache" 模块
	luaL_requiref(L, "skynet.codecache", codecache , 0);
	lua_pop(L,1);

	// 设置 LUA_PATH 全局变量
	const char *path = optstring(ctx, "lua_path","./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);
	lua_setglobal(L, "LUA_PATH");

	// 设置 LUA_CPATH 全局变量
	const char *cpath = optstring(ctx, "lua_cpath","./luaclib/?.so");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");

	// 设置 LUA_SERVICE 全局变量
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);
	lua_setglobal(L, "LUA_SERVICE");

	// 设置 LUA_PRELOAD 全局变量
	const char *preload = skynet_command(ctx, "GETENV", "preload");
	lua_pushstring(L, preload);
	lua_setglobal(L, "LUA_PRELOAD");

	// 压入错误跟踪函数
	lua_pushcfunction(L, traceback);
	assert(lua_gettop(L) == 1);

	// 加载 loader.lua 的代码块, 但没有运行.
	const char * loader = optstring(ctx, "lualoader", "./lualib/loader.lua");
	int r = luaL_loadfile(L,loader);
	if (r != LUA_OK) {
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));
		_report_launcher_error(ctx);
		return 1;
	}

	// 压入参数, 执行 loader.lua 代码块
	lua_pushlstring(L, args, sz);
	r = lua_pcall(L,1,0,1);
	if (r != LUA_OK) {
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		_report_launcher_error(ctx);
		return 1;
	}

	// 清空栈
	lua_settop(L,0);

	lua_gc(L, LUA_GCRESTART, 0);	// 重启垃圾收集器

	return 0;
}

static int
_launch(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz) {
	assert(type == 0 && session == 0);
	struct snlua *l = ud;

	// 注意, 在接收 1 次消息后, 马上取消消息处理函数
	skynet_callback(context, NULL, NULL);

	int err = _init(l, context, msg, sz);
	if (err) {
		skynet_command(context, "EXIT", NULL);
	}

	return 0;
}

/// 初始化 struct snlua
int
snlua_init(struct snlua *l, struct skynet_context *ctx, const char * args) {
	// 复制参数数据
	int sz = strlen(args);
	char * tmp = skynet_malloc(sz);
	memcpy(tmp, args, sz);

	skynet_callback(ctx, l, _launch);

	// 获取当前 skynet_context 的 handle
	const char * self = skynet_command(ctx, "REG", NULL);
	uint32_t handle_id = strtoul(self+1, NULL, 16);
	
	// it must be first message
	// 这必须是第一个消息
	skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY, 0, tmp, sz);
	return 0;
}

/// 创建 struct snlua
struct snlua *
snlua_create(void) {
	struct snlua * l = skynet_malloc(sizeof(*l));
	memset(l,0,sizeof(*l));
	l->L = lua_newstate(skynet_lalloc, NULL);
	return l;
}

/// 释放 struct snlua
void
snlua_release(struct snlua *l) {
	lua_close(l->L);
	skynet_free(l);
}

/// struct snlua 对于信号量处理
void
snlua_signal(struct snlua *l, int signal) {
	skynet_error(l->ctx, "recv a signal %d", signal);
#ifdef lua_checksig
	// If our lua support signal (modified lua version by skynet), trigger it.
	// 如果我们的 lua 支持信号(修改的 lua 版本), 触发它.
	skynet_sig_L = l->L;
#endif
}
