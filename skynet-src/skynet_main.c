#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"
#include "luashrtbl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

/// 得到 key 对应的整型数据, 如果之前没有赋值, 那么默认使用 opt
static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

/*
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}
*/

/// 得到 key 对应的字符串数据, 如果之前没有赋值, 那么默认使用 opt
static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

/// 初始化全局配置变量, 从 L 从拿出数据, 传入到 skynet_setenv 中
static void
_init_env(lua_State *L) {
	lua_pushnil(L);  /* first key */
	while (lua_next(L, -2) != 0) {	// 查看 lua_next 函数, 了解具体的意思, http://cloudwu.github.io/lua53doc/manual.html
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}

		// 拿到键
		const char * key = lua_tostring(L,-2);

		// 拿到值
		if (lua_type(L, -1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key, b ? "true" : "false");
		} else {
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key, value);
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

// 这个函数主要是为了避免这个问题 http://www.cnblogs.com/caosiyang/archive/2012/07/19/2599071.html
/// 忽略掉 SIGPIPE 信号.
int sigign() {
	/*
	http://www.cnblogs.com/wblyuyang/archive/2012/11/13/2768923.html

	struct sigaction 类型用来描述对信号的处理，定义如下：
	struct sigaction
	{
		void     (*sa_handler)(int);		// 是一个函数指针，其含义与 signal 函数中的信号处理函数类似
		void     (*sa_sigaction)(int, siginfo_t *, void *);	// 是另一个信号处理函数，它有三个参数，可以获得关于信号的更详细的信息。
		sigset_t  sa_mask;	// 用来指定在信号处理函数执行期间需要被屏蔽的信号，特别是当某个信号被处理时，它自身会被自动放入进程的信号掩码，因此在信号处理函数执行期间这个信号不会再度发生。
		int       sa_flags;		// 当 sa_flags 成员的值包含了 SA_SIGINFO 标志时，系统将使用 sa_sigaction 函数作为信号处理函数，否则使用 sa_handler 作为信号处理函数。
		void     (*sa_restorer)(void);	// 此参数没有使用
	};

	*/

	struct sigaction sa;
	sa.sa_handler = SIG_IGN;

	// int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
	// 依参数 signum 指定的信号编号来设置该信号的处理函数。
	// signum：要操作的信号。
	// act：要设置的对信号的新处理方式。
	// oldact：原来对信号的处理方式。
	// 返回值：0 表示成功，-1 表示有错误发生。
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

/// 这部分的 lua 代码表示, 替换配置文件中使用了环境变量的值(例如: $SKYNET_THREAD)替换成实际的值.
static const char * load_config = "\
	local config_name = ...\
	local f = assert(io.open(config_name))\
	local code = assert(f:read \'*a\')\
	local function getenv(name) return assert(os.getenv(name), \'os.getenv() failed: \' .. name) end\
	code = string.gsub(code, \'%$([%w_%d]+)\', getenv)\
	f:close()\
	local result = {}\
	assert(load(code,\'=(load)\',\'t\',result))()\
	return result\
";

int
main(int argc, char *argv[]) {

	// 拿到传入的第一个参数, 配置文件
	const char * config_file = NULL ;
	if (argc > 1) {
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	luaS_initshr();

	skynet_globalinit();

	skynet_env_init();

	sigign();

	struct skynet_config config;

	struct lua_State *L = lua_newstate(skynet_lalloc, NULL);
	luaL_openlibs(L);	// link lua lib

	// 加载 config 文件里面的内容, 使用 skynet_setenv 函数, 将数据配置进来
	int err = luaL_loadstring(L, load_config);
	assert(err == LUA_OK);
	lua_pushstring(L, config_file);
	err = lua_pcall(L, 1, 1, 0);		// 这个时候栈顶元素是 table 类型, 就是 load_config 返回的 result
	if (err) {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		lua_close(L);
		return 1;
	}
	_init_env(L);

	// 配置初始化
	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);
	config.logservice = optstring("logservice", "logger");

	lua_close(L);

	// 启动 skynet 节点
	skynet_start(&config);

	skynet_globalexit();
	luaS_exitshr();

	return 0;
}
