#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

// stdlib 头文件即standard library标准库头文件。stdlib.h里面定义了五种类型、一些宏和通用工具函数。
#include <stdlib.h>

// assert.h常用于防御式编程。断言（Assertions），一个断言通常是一个例程（routines）或者一个宏（marcos）。
// 每个断言通常含有两个参数：一个布尔表示式（a boolean expression）和一个消息（a message）。
#include <assert.h>

struct skynet_env {
<<<<<<< HEAD
	struct spinlock lock;		// 简单的多线程锁
	lua_State *L;	// lua 沙箱
=======
	struct spinlock lock;
	lua_State *L;
>>>>>>> cloudwu/master
};

static struct skynet_env *E = NULL;

const char * 
skynet_getenv(const char *key) {
	SPIN_LOCK(E)

	lua_State *L = E->L;
	
	lua_getglobal(L, key);
	const char * result = lua_tostring(L, -1);
	lua_pop(L, 1);

	SPIN_UNLOCK(E)

	return result;
}

void 
skynet_setenv(const char *key, const char *value) {
	SPIN_LOCK(E)
	
	lua_State *L = E->L;
	lua_getglobal(L, key);
	assert(lua_isnil(L, -1));
	lua_pop(L,1);
	lua_pushstring(L,value);
	lua_setglobal(L,key);

	SPIN_UNLOCK(E)
}

void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	SPIN_INIT(E)
	E->L = luaL_newstate();
}
