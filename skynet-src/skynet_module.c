#include "skynet.h"

#include "skynet_module.h"

#include <assert.h>
#include <string.h>

// Linux动态库的显式调用
// 学习Linux，你可能会遇到动态库的显式调用，这里介绍动态库的显式调用的解决方法。
// 显式调用的含义是代码出现库文件名，用户需要自己去打开和管理库文件。
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

// 设定模块种类
#define MAX_MODULE_TYPE 32

struct modules {
	int count;		// 统计当前模块的数量
	int lock;		// 简单的多线程锁, 不能嵌套
	const char * path;	// 模块的搜索路径
	struct skynet_module m[MAX_MODULE_TYPE];	// skynet_module 数组
};

static struct modules * M = NULL;

// 尝试打开动态链接库
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;

	// 得到动态链接库路径的字符串大小
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);
	int sz = path_size + name_size;
	
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);

		// 因为每个查询路径都是使用 ';' 分割, 所以查到 ';' 之后的第一个字符
		while (*path == ';') path++;

		// 如果 ';' 之后没有可查询的路径, 那么则认为查询结束
		if (*path == '\0') break;

		// 查找接下来会出现 ';' 的地方
		l = strchr(path, ';');

		// 得到当前搜索路径的总长度
		if (l == NULL) l = path + strlen(path);
		int len = l - path;

		// 将路径的值复制给 tmp
		int i;
		for (i = 0; path[i] != '?' && i < len; i++) {
			tmp[i] = path[i];
		}

		// tmp 后面追加文件名
		memcpy(tmp + i, name, name_size);

		// path 中的通用文件名必须是 '?' 表示
		if (path[i] == '?') {
			// 将动态链接库的后缀连接上
			strncpy(tmp + i + name_size, path + i + 1, len - i - 1);
		} else {
			// 格式不正确
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}

		// 尝试打开动态链接库
		// 打开一个动态链接库，并返回动态链接库的句柄
		// void * dlopen( const char * pathname, int mode);
		// pathname: 文件名
		// mode: 打开方式，其值有多个，不同操作系统上实现的功能有所不同，在linux下，按功能可分为三类：
		// 1、解析方式
		// 		RTLD_LAZY：在 dlopen 返回前，对于动态库中的未定义的符号不执行解析（只对函数引用有效，对于变量引用总是立即解析）。
		// 		RTLD_NOW： 需要在dlopen返回前，解析出所有未定义符号，如果解析不出来，在dlopen会返回NULL，错误为：: undefined symbol: xxxx.......
		// 2、作用范围，可与解析方式通过“|”组合使用。
		// 		RTLD_GLOBAL：动态库中定义的符号可被其后打开的其它库解析。
		// 		RTLD_LOCAL： 与RTLD_GLOBAL作用相反，动态库中定义的符号不能被其后打开的其它库重定位。如果没有指明是RTLD_GLOBAL还是RTLD_LOCAL，则缺省为RTLD_LOCAL。
		// 3、作用方式
		// 		RTLD_NODELETE： 在dlclose()期间不卸载库，并且在以后使用dlopen()重新加载库时不初始化库中的静态变量。这个flag不是POSIX-2001标准。
		// 		RTLD_NOLOAD： 不加载库。可用于测试库是否已加载(dlopen()返回NULL说明未加载，否则说明已加载），也可用于改变已加载库的flag，如：先前加载库的flag为RTLD_LOCAL，用dlopen(RTLD_NOLOAD|RTLD_GLOBAL)后flag将变成RTLD_GLOBAL。这个flag不是POSIX-2001标准。
		// 		RTLD_DEEPBIND：在搜索全局符号前先搜索库内的符号，避免同名符号的冲突。这个flag不是POSIX-2001标准。
		// 返回值: 打开错误返回NULL, 成功，返回库引用.
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);

		// 从下一个搜索路径开始查找
		path = l;
	}while(dl == NULL);

	// 如果搜索完整个路径都没找到对应的动态链接库, 输出错误信息
	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n", name, dlerror());
	}

	return dl;
}

/// 根据模块的名字查询到对象的 skynet_module 对象
/// 如果查询到返回对应 skynet_module 指针, 否则返回 NULL.
static struct skynet_module * 
_query(const char * name) {
	int i;
	// 对当前已经插入的模块数组进行查询
	for (i = 0; i < M->count; i++) {
		if (strcmp(M->m[i].name, name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

/// 将已经打开的动态链接库的 xxx_create, xxx_init, xxx_release, xxx_signal 方法导入.
/// 返回 1 表示初始化失败, 返回 0 表示初始化成功.
static int
_open_sym(struct skynet_module *mod) {

	// 分配内存空间
	size_t name_size = strlen(mod->name);
	char tmp[name_size + 9]; // create/init/release/signal , longest name is release (7)
	memcpy(tmp, mod->name, name_size);

	// dlsym 介绍
	// void * dlsym(void * handle, const char * symbol)
	// 根据 动态链接库 操作句柄(handle)与符号(symbol)，返回符号对应的地址。使用这个函数不但可以获取函数地址，也可以获取变量地址。
	// handle：由dlopen打开动态链接库后返回的指针；
	// symbol：要求获取的函数或全局变量的名称。
	// void* 指向函数的地址，供调用使用。

	// 获得 xxxx_create 函数, 模块函数在定义的时候也必须按照这种格式来命名
	strcpy(tmp+name_size, "_create");
	mod->create = dlsym(mod->module, tmp);

	// 获得 xxxx_init 函数
	strcpy(tmp+name_size, "_init");
	mod->init = dlsym(mod->module, tmp);

	// 获得 xxxx_release 函数
	strcpy(tmp+name_size, "_release");
	mod->release = dlsym(mod->module, tmp);

	// 获得 xxxx_signal 函数
	strcpy(tmp+name_size, "_signal");
	mod->signal = dlsym(mod->module, tmp);

	// mod->init 方法是必须要实现的
	return mod->init == NULL;
}

struct skynet_module * 
skynet_module_query(const char * name) {

	// 首先查询, 判断是否已经加载了模块
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	// 如果没有加载, 保证线程的安全性
	while(__sync_lock_test_and_set(&M->lock,1)) {}

	// 再查询一次, 判断是否其他线程有加载这个模块
	result = _query(name); // double check
	
	// 保证空间足够, 创建 skynet_module, 并加入到集合中
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;
		void * dl = _try_open(M,name);
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;

			if (_open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	__sync_lock_release(&M->lock);

	return result;
}

void 
skynet_module_insert(struct skynet_module *mod) {

	// 保证线程安全
	while(__sync_lock_test_and_set(&M->lock,1)) {}

	// 保证 mod 之前并没有插入到集合中
	struct skynet_module * m = _query(mod->name);
	assert(m == NULL && M->count < MAX_MODULE_TYPE);

	// 将 mod 加入到集合中
	int index = M->count;
	M->m[index] = *mod;
	++M->count;

	__sync_lock_release(&M->lock);
}

void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
	m->path = skynet_strdup(path);
	m->lock = 0;

	M = m;
}
