#include "skynet.h"
#include "skynet_env.h"

#include <lua.h>
#include <lauxlib.h>

// stdlib 头文件即standard library标准库头文件。stdlib.h里面定义了五种类型、一些宏和通用工具函数。
#include <stdlib.h>

// assert.h常用于防御式编程。断言（Assertions），一个断言通常是一个例程（routines）或者一个宏（marcos）。
// 每个断言通常含有两个参数：一个布尔表示式（a boolean expression）和一个消息（a message）。
#include <assert.h>

struct skynet_env {
	int lock;		// 简单的多线程锁
	lua_State *L;	// lua 沙箱
};

static struct skynet_env *E = NULL;

// 上锁
#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}

// 解锁
#define UNLOCK(q) __sync_lock_release(&(q)->lock);

const char * 
skynet_getenv(const char *key) {

	// 保证线程的安全
	LOCK(E)

	lua_State *L = E->L;
	
	lua_getglobal(L, key);
	const char * result = lua_tostring(L, -1);
	lua_pop(L, 1);

	UNLOCK(E)

	return result;
}

void 
skynet_setenv(const char *key, const char *value) {
	LOCK(E)
	
	lua_State *L = E->L;
	lua_getglobal(L, key);
	assert(lua_isnil(L, -1));
	lua_pop(L,1);
	lua_pushstring(L,value);
	lua_setglobal(L,key);

	UNLOCK(E)
}

void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	E->lock = 0;
	E->L = luaL_newstate();
}
