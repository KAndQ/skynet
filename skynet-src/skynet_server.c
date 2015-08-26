#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"
#include "spinlock.h"
#include "atomic.h"

// POSIX线程（POSIX threads），简称 pthreads，是线程的 POSIX 标准。该标准定义了创建和操纵线程的一整套API。
// 在类Unix操作系统（Unix、Linux、Mac OS X等）中，都使用 pthreads 作为操作系统的线程。
#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

// C99 才有的 C 里面有 bool 类型
#include <stdbool.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
#define CHECKCALLING_END(ctx) spinlock_unlock(&ctx->calling);
#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
#define CHECKCALLING_DESTROY(ctx) spinlock_destroy(&ctx->calling);
#define CHECKCALLING_DECL struct spinlock calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DESTROY(ctx)
#define CHECKCALLING_DECL

#endif

// skynet 的核心数据结构之一
struct skynet_context {
	void * instance;				// skynet_module 对应的动态链接库创建的实例
	struct skynet_module * mod;		// 关联的模块
	void * cb_ud;					// 回调的参数
	skynet_cb cb;					// 回调的函数
	struct message_queue *queue;	// 消息队列
	FILE * logfile;					// 日志文件句柄
	char result[32];				// 将 cmd_xxx 运算的一些值存储在 result 里面
	uint32_t handle;				// 在当前 skynet 节点中的 handle, 由 skynet_handle 分配
	int session_id;					// session 的累计计数
	int ref;						// 引用计数
	bool init;						// 是否初始化
	bool endless;					// 标记当前 context 处理消息的时候是不是进入了死循环(也有可能计算消耗的时间过长)

	CHECKCALLING_DECL
};

// skynet 网络节点
struct skynet_node {
	int total;						// skynet_context 的 总数量
	int init;						// 是否初始化
	uint32_t monitor_exit;			// 每个 skynet_context 在 exit 时会发送消息给 monitor_exit 对应的 skynet_context

	// 概念及作用
	// 在单线程程序中，我们经常要用到"全局变量"以实现多个函数间共享数据。在多线程环境下，由于数据空间是共享的，因此全局变量也为所有线程所共有。
	// 但有时应用程序设计中有必要提供线程私有的全局变量，仅在某个线程中有效，但却可以跨多个函数访问，比如程序可能需要每个线程维护一个链表，而使用相同的函数操作，
	// 最简单的办法就是使用同名而不同变量地址的线程相关数据结构。这样的数据结构可以由Posix线程库维护，称为线程私有数据（Thread-specific Data，或TSD）。

	// 在 skynet_initthread 时使用的是对应线程号(skynet_imp.h 中定义) 来初始化 handle_key 的值.
	// 在调用过 dispatch_message 方法后, handle_key 保存的是当前线程调用 dispatch_message 时传入的 skynet_context 的 handle 值
	pthread_key_t handle_key;
};

static struct skynet_node G_NODE;

int 
skynet_context_total() {
	return G_NODE.total;
}

// G_NODE.total 计数加 1
static void
context_inc() {
	ATOM_INC(&G_NODE.total);
}

// G_NODE.total 计数减 1
static void
context_dec() {
	ATOM_DEC(&G_NODE.total);
}

uint32_t 
skynet_current_handle(void) {
	if (G_NODE.init) {
		// 已经初始化过:
		// 1. 那么得到各个线程对应的值
		// 2. 在调用过 dispatch_message 方法后, handle_key 保存的是当前线程调用 dispatch_message 时传入的 skynet_context 的 handle 值
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {

		// 没有初始化, 则返回 0xffffffff
		uint32_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}

// 将 id 以这样格式 ":FF123456" 的字符串返回. 冒号之后的是 16 进制数据.
static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[9] = '\0';
}

// issues!!!
struct drop_t {
	uint32_t handle;
};

// 把 skynet_message 管理的 data 数据内容释放掉
static void
drop_message(struct skynet_message *msg, void *ud) {
	struct drop_t *d = ud;
	skynet_free(msg->data);
	uint32_t source = d->handle;
	assert(source);

	// report error to the message source
	// 报告错误给 msg->source, 
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

struct skynet_context * 
skynet_context_new(const char * name, const char * param) {
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;

	void * inst = skynet_module_instance_create(mod);
	if (inst == NULL)
		return NULL;

	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	CHECKCALLING_INIT(ctx)

	ctx->mod = mod;
	ctx->instance = inst;
	ctx->ref = 2;
	ctx->cb = NULL;
	ctx->cb_ud = NULL;
	ctx->session_id = 0;
	ctx->logfile = NULL;

	ctx->init = false;
	ctx->endless = false;
	
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	// 首先应该设置 handle 为 0, 避免 skynet_handle_retireall 方法得到一个未初始化的 handle
	ctx->handle = 0;
	ctx->handle = skynet_handle_register(ctx);	// 当 ctx->handle 被赋值的时候, 才能保证 handle 的管理器 和 context 同步了.

	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);

	// 全局引用计数 +1
	context_inc();

	// init function maybe use ctx->handle, so it must init at last
	// init 函数可能会使用 ctx->handle, 所以它必须在最后初始化
	CHECKCALLING_BEGIN(ctx)
	int r = skynet_module_instance_init(mod, inst, ctx, param);
	CHECKCALLING_END(ctx)

	// inst 初始化成功
	if (r == 0) {
		// ctx 引用 -1
		struct skynet_context * ret = skynet_context_release(ctx);

		// 标记初始化成功
		if (ret) {
			ctx->init = true;
		}

		// 将 queue 加入到 global_queue 的链表里面
		skynet_globalmq_push(queue);

		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;

	// inst 初始化失败
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;

		// ctx 引用 -1
		skynet_context_release(ctx);

		// ctx 引用 -1, 内存资源被释放, 这里会调用 delete_context 方法, 函数里面实现了 skynet_mq_mark_release
		skynet_handle_retire(handle);

		// 将消息队列里面的资源全部释放
		struct drop_t d = { handle };
		skynet_mq_release(queue, drop_message, &d);

		return NULL;
	}
}

int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	// seesion 必须为正
	int session = ++ctx->session_id;
	if (session <= 0) {
		ctx->session_id = 1;
		return 1;
	}
	return session;
}

void 
skynet_context_grab(struct skynet_context *ctx) {
	// ctx 的引用 +1
	ATOM_INC(&ctx->ref);
}

void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0.
	// the reserved context will be release at last.
	
	// 不要统计保留的 context, 因为 skynet 只有在 G_NODE 管理的 skynet_context 为 0 的时候终止(工作的线程被终止).
	// 保留的 context 将在最后被释放.
	// issue: 这里我的理解是, 认为传入的 ctx 是系统默认存在的, 不需要统计. 目前只有在 skynet_harbor_start 中调用过, 那么可以认为
	// harbor 关联的这个 ctx 是当前节点默认存在的, 意思可以理解为, 我需要对 ctx 做引用计数, 但是这个 ctx 不要求进入 G_NODE 的统计.
	context_dec();
}

// 删除 ctx 实例
static void 
delete_context(struct skynet_context *ctx) {
	// 关闭关联的日志文件
	if (ctx->logfile) {
		fclose(ctx->logfile);
	}

	// 关联的模块实例资源释放
	skynet_module_instance_release(ctx->mod, ctx->instance);

	// 标记关联的 message_queue 为释放状态, message_queue 的实际删除会在 skynet_context_message_dispatch 中执行
	skynet_mq_mark_release(ctx->queue);

	CHECKCALLING_DESTROY(ctx)

	// 释放内存资源
	skynet_free(ctx);

	// 全局引用计数 -1
	context_dec();
}

struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {

	// 引用计数为 0 的时候, 自动释放掉 ctx
	if (ATOM_DEC(&ctx->ref) == 0) {
		delete_context(ctx);
		return NULL;
	}
	return ctx;
}

int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
	// 先保留一个引用
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return -1;
	}

	// 将 message 压入到 context->queue 中
	skynet_mq_push(ctx->queue, message);

	// 释放保留
	skynet_context_release(ctx);

	return 0;
}

void 
skynet_context_endless(uint32_t handle) {
	// 保留一个引用
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}

	// 打个死循环的标记
	ctx->endless = true;

	// 释放保留
	skynet_context_release(ctx);
}

int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {
	// 得到 handle 表示的是否是远程节点的结果
	int ret = skynet_harbor_message_isremote(handle);

	// 获取 handle 对应的 harbor id, 高 8 位
	if (harbor) {
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}

	return ret;
}

/// skynet_context 对单个 skynet_message 做处理
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	// 保证初始化
	assert(ctx->init);

	CHECKCALLING_BEGIN(ctx)

	// 存储当前线程, dispatch message 的 skynet_context 的 handle
	// 这样在 ctx->cb 中也可以得到当前线程正在 dispatch message 的 skynet_context(handle)
	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));

	// 拿到消息类型, 高 8 位存储
	int type = msg->sz >> MESSAGE_TYPE_SHIFT;

	// 拿到数据的大小
	size_t sz = msg->sz & MESSAGE_TYPE_MASK;

	// 如果当前服务有日志文件, 将信息输出到日志文件中, 这是每个服务特有的日志文件
	if (ctx->logfile) {
		skynet_log_output(ctx->logfile, msg->source, type, msg->session, msg->data, sz);
	}

	// 每个 skynet_context 处理 skynet_message
	if (!ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz)) {
		// 消息处理成功释放掉 skynet_message 的 data 内存资源
		// 注意, 在调用本方法(dispatch_message) 之前, 消息队列已经将 msg 从 queue 里面 pop 出来了.
		// 如果这里不释放掉 message 的 data 资源, 那么将会造成内存泄漏.
		skynet_free(msg->data);
	} 
	CHECKCALLING_END(ctx)
}

void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	// 用于 skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	while (!skynet_mq_pop(q,&msg)) {
		dispatch_message(ctx, &msg);
	}
}

struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	if (q == NULL) {
		// 从全局消息队列中弹出一个 message_queue
		q = skynet_globalmq_pop();

		// 如果当前一个 message_queue 都没, 那么函数返回
		if (q == NULL)
			return NULL;
	}

	// 拿到 message_queue 的 handle
	uint32_t handle = skynet_mq_handle(q);

	// 在使用 handle 拿到对应的 context, 保留引用
	struct skynet_context * ctx = skynet_handle_grab(handle);

	// 如果不存在对应的 skynet_context, 使用另外的 message_queue
	if (ctx == NULL) {
		struct drop_t d = { handle };

		// 1. 如果 q 没有打 release 标记, 那么 skynet_mq_release 会将 message_queue 再压入到 global_queue 里面.
		// 2. 如果 q 打了 release 标记, 那么 skynet_mq_release 会将 message_queue 里面的 skynet_message 资源全部释放掉, 同时释放掉 message_queue 自身
		skynet_mq_release(q, drop_message, &d);

		// 弹出下一个链表头元素
		return skynet_globalmq_pop();
	}

	int i, n = 1;
	struct skynet_message msg;

	for (i = 0; i < n; i++) {
		
		// 从 mq 里面弹出 1 个 skynet_message
		if (skynet_mq_pop(q, &msg)) {	// 弹出消息失败

			// 释放引用
			skynet_context_release(ctx);

			// 拿到下一个链头元素
			return skynet_globalmq_pop();

		// 决定接下来循环的次数
		} else if (i == 0 /* 第一次循环 */ && weight >= 0 /* 值要有意义 */ ) {		// 弹出消息成功
			n = skynet_mq_length(q);
			n >>= weight;	// >> 每右移一位表示除 2
		}

		// 判断当前 queue 存储的消息数量是否超载了
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		// 开启监控器
		skynet_monitor_trigger(sm, msg.source, handle);

		// 如果没有处理函数, 那么直接将 skynet_message 的内存资源释放
		if (ctx->cb == NULL) {
			skynet_free(msg.data);

		// context 处理 skynet_message 消息
		} else {
			dispatch_message(ctx, &msg);
		}

		// 关闭监控器
		skynet_monitor_trigger(sm, 0, 0);
	}

	// 保证处理 context->queue 和 q 是相同的
	assert(q == ctx->queue);

	// 弹出下次使用的 message_queue, 或者继续使用当前的 message_queue
	// 这里决定如果当前的 message_queue 消息没有处理完, 接下来会如何处理
	struct message_queue * nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty, push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
		// 如果 global_queue 不是空的, 再将当前的 message_queue(q) 放入到链表的尾部, 返回下一个 message_queue(nq)
		// 否则如果 global_queue 是空, 将不再把 message_queue(q) 压入到尾部, 随后再一次返回 message(q)(为下一次 dispatch)
		skynet_globalmq_push(q);
		q = nq;
	}

	// 释放引用
	skynet_context_release(ctx);

	return q;
}

/// 将 addr 的内容复制到 name 里面
static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i = 0; i < GLOBALNAME_LENGTH && addr[i]; i++) {
		name[i] = addr[i];
	}

	for (; i<GLOBALNAME_LENGTH; i++) {
		name[i] = '\0';
	}
}

uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
		// 将字符串转换成无符号长整型数
		return strtoul(name + 1, NULL, 16);
	case '.':
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s", name);
	return 0;
}

/// 撤销 handle 对应的 skynet_context. 如果 handle == 0, 那么撤销的是传入的 skynet_context.
static void
handle_exit(struct skynet_context * context, uint32_t handle) {

	// 决定撤销的 handle, 并且打印信息
	if (handle == 0) {
		handle = context->handle;
		skynet_error(context, "KILL self");
	} else {
		skynet_error(context, "KILL :%0x", handle);
	}

	// 发送消息给 moniter_exit 对应的 skynet_context
	if (G_NODE.monitor_exit) {
		skynet_send(context, handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}

	// 撤销 handle
	skynet_handle_retire(handle);
}

// skynet command
// skynet 命令

struct command_func {
	const char *name;	// 命令的名字
	const char * (*func)(struct skynet_context * context, const char * param);	// 对应的回调函数接口声明
};

/// 给 context 添加计时器, skynet.timeout 用到这个命令
static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;

	// 一开始strtol()会扫描参数nptr字符串，跳过前面的空格字符，直到遇上数字或正负符号才开始做转换，
	// 再遇到非数字或字符串结束时('\0')结束转换，并将结果返回。
	// 若参数endptr不为NULL，则会将遇到不合条件而终止的nptr中的字符指针由endptr返回；若参数endptr为NULL，则会不返回非法字符串。
	int ti = strtol(param, &session_ptr, 10);

	// 生成信息的 session id
	int session = skynet_context_newsession(context);

	// 添加计时器
	skynet_timeout(context->handle, ti, session);

	// 记录结果数据
	sprintf(context->result, "%d", session);
	return context->result;
}

/// 在本节点内, 给 context 注册一个名字, skynet.register 和 skynet.self 用到这个命令
static const char *
cmd_reg(struct skynet_context * context, const char * param) {
	// 如果没有 param 参数或者 param 为空字符串,
	// 那么结果是 ":" + context->handle 的 16 进制字符串
	if (param == NULL || param[0] == '\0') {
		sprintf(context->result, ":%x", context->handle);
		return context->result;

	// 在本地服务给 context 注册名字, 名字开头符号必须是'.'
	// 注意: 注册的名字是已经省略掉前面的 . 符号了
	} else if (param[0] == '.') {
		return skynet_handle_namehandle(context->handle, param + 1);

	// 不支持注册全局(整个 skynet 网络)名字
	} else {
		skynet_error(context, "Can't register global name %s in C", param);
		return NULL;
	}
}

/// 在本节点内, 根据服务的名字查询, skynet.localname 和 skynet.queryservice 方法使用这个命令
static const char *
cmd_query(struct skynet_context * context, const char * param) {
	if (param[0] == '.') {

		// 查询到名字对应的 handle
		uint32_t handle = skynet_handle_findname(param+1);
		if (handle) {

			// 记录结果数据
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

/// 在本节点内, 给 handle 注册一个名字, skynet.name 用到这个命令.
static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size + 1];
	char handle[size + 1];

	// 从一个字符串中读进与指定格式相符的数据。
	sscanf(param, "%s %s", name, handle);

	// handle 格式判断
	if (handle[0] != ':') {
		return NULL;
	}

	// 拿到 handle 值
	uint32_t handle_id = strtoul(handle + 1, NULL, 16);

	// handle_id 校验
	if (handle_id == 0) {
		return NULL;
	}

	// name 格式判断
	if (name[0] == '.') {
		return skynet_handle_namehandle(handle_id, name + 1);

	// 不能以全局格式命名
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}

	return NULL;
}

/// 得到 skynet 节点从启动到目前的系统时间, skynet.now() 用到这个命令.
static const char *
cmd_now(struct skynet_context * context, const char * param) {
	uint32_t ti = skynet_gettime();
	sprintf(context->result, "%u", ti);
	return context->result;
}

/// 撤销当前 skynet_context, skynet.exit 中会使用到.
static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

/// 将 param 表示的字符串转化为对应的 handle
static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	// 从数字转化
	if (param[0] == ':') {
		handle = strtoul(param + 1, NULL, 16);
	// 从名字转化
	} else if (param[0] == '.') {
		handle = skynet_handle_findname(param + 1);
	} else {
		skynet_error(context, "Can't convert %s to handle", param);
	}

	return handle;
}

/// 撤销掉指定的 skynet_context, skynet.kill 中会使用到
static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

/// 从本地加载动态链接库, 创建一个新的 skynet_context, skynet.launch 中会使用到
static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	
	// 复制 param 的内容
	size_t sz = strlen(param);
	char tmp[sz + 1];
	strcpy(tmp, param);

	char * args = tmp;

	// 得到第一个参数字符串
	char * mod = strsep(&args, " \t\r\n");

	// 得到第二个参数字符串
	args = strsep(&args, "\r\n");

	// 加载新的 skynet_context 模块
	struct skynet_context * inst = skynet_context_new(mod, args);

	if (inst == NULL) {
		return NULL;
	} else {
		// 将新创建的 inst->handle 以 16 进制字符串的形式记录下来
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

/// 获得当前节点的全局环境变量, skynet.getenv 中使用
static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

/// 设置当前节点的全局环境变量, skynet.setenv 中使用
static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char key[sz + 1];
	
	// 拿到第一个参数
	int i;
	for (i = 0; param[i] != ' ' && param[i]; i++) {
		key[i] = param[i];
	}

	// 保证 param 后面还是数据
	if (param[i] == '\0')
		return NULL;

	// key 字符串
	key[i] = '\0';

	// value 字符串
	param += i + 1;
	
	// 设置
	skynet_setenv(key, param);
	
	return NULL;
}

/// 得到 skynet 节点启动的系统时间, skynet.starttime() 中有使用到
static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	uint32_t sec = skynet_gettime_fixsec();
	sprintf(context->result,"%u",sec);
	return context->result;
}

/// 得到 context 的 endless 标记值, skynet.endless() 中有使用到
static const char *
cmd_endless(struct skynet_context * context, const char * param) {
	if (context->endless) {
		strcpy(context->result, "1");
		context->endless = false;
		return context->result;
	}
	return NULL;
}

/// 关闭掉所有的 skynet_context, skynet.abord() 中有使用到
static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

// 添加 skynet_context 在关闭时的监控器, 这个监控器其实也是一个 skynet_context.
// skynet.monitor 中有使用到.
static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	if (param == NULL || param[0] == '\0') {
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			// 返回当前的监控服务的 handle
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

/// 得到当前 skynet_context 的 queue 长度, skynet.mqlen() 中有使用到
static const char *
cmd_mqlen(struct skynet_context * context, const char * param) {
	int len = skynet_mq_length(context->queue);
	sprintf(context->result, "%d", len);
	return context->result;
}

// 打开指定 skynet_context 的日志文件, 指定的 skynet_context 通过 param 传入 handle 拿到.
// 在 command.LOGLAUNCH 和 COMMAND.logon 中有使用到.
static const char *
cmd_logon(struct skynet_context * context, const char * param) {
	// 拿到 handle
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;

	// 拿到 skynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;

	FILE * f = NULL;
	FILE * lastf = ctx->logfile;

	// 保证之前没有打开过文件
	if (lastf == NULL) {
		// 打开 handle 对应的 skynet_context 的日志文件
		f = skynet_log_open(context, handle);
		if (f) {
			// __sync_bool_compare_and_swap: 如果第 1 个参数和第 2 个参数值相等, 那么把第 3 个参数的值赋给第 1 个参数.
			if (!ATOM_CAS_POINTER(&ctx->logfile, NULL, f)) {
				// logfile opens in other thread, close this one.
				// 如果日志文件已经被其他线程打开了, 那么关闭掉当前的.
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);

	return NULL;
}

// 关闭指定 skynet_context 的日志文件, 指定的 skynet_context 通过 param 传入 handle 拿到.
// 在 COMMAND.logoff 中使用到.
static const char *
cmd_logoff(struct skynet_context * context, const char * param) {
	// 拿到 handle
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;

	// 拿到 skynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;

	FILE * f = ctx->logfile;
	if (f) {
		// logfile may close in other thread
		// 日志文件可能在其他线程被关掉
		if (ATOM_CAS_POINTER(&ctx->logfile, f, NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

// 对指定的 skynet_context 调用 skynet_module_instance_signal 方法.
// 在 COMMAND.signal 中使用到.
static const char *
cmd_signal(struct skynet_context * context, const char * param) {
	// 拿到 handle
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;

	// 拿到 skynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;

	// 拿到 sign 参数值
	param = strchr(param, ' ');
	int sig = 0;
	if (param) {
		sig = strtol(param, NULL, 0);
	}

	// NOTICE: the signal function should be thread safe.
	// 注意: 这个信号函数应该保证是线程安全的.
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);

	return NULL;
}

static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "NOW", cmd_now },
	{ "EXIT", cmd_exit },
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ENDLESS", cmd_endless },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "MQLEN", cmd_mqlen },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ "SIGNAL", cmd_signal },
	{ NULL, NULL },
};

const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];

	// 遍历搜索
	while(method->name) {
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);
		}
		++method;
	}

	return NULL;
}

/// 对传入的参数进行过滤, session, data, sz
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);		// 复制
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;	// 分配 session

	// 拿到当前的 PTYPE_*
	type &= 0xff;

	// 分配一个新的 session
	if (allocsession) {
		assert(*session == 0);
		*session = skynet_context_newsession(context);
	}

	// 复制数据, 将 *data 的数据复制到新分配的内存空间中
	if (needcopy && *data) {
		char * msg = skynet_malloc(*sz + 1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	// 高 8 位存储 PTYPE_*
	*sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}

int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {
	
	// 数据大小的判断
	if ((sz & MESSAGE_TYPE_MASK) != sz) {
		skynet_error(context, "The message to %x is too large", destination);
		skynet_free(data);
		return -1;
	}

	// 参数过滤, 这个函数会根据 type 的值申请新的内存空间.
	// issue: 如果对 data 进行了复制, 那么老的 data 资源由谁来管理呢?
	// #a-issue: 这个需要视情况来决定, 因为有些时候这个 data 指针的数据并不由 skynet 框架管理, 例如 lua 的字符串.
	_filter_args(context, type, &session, (void **)&data, &sz);

	// 如果 source 为 0, 表示用 context 发送
	if (source == 0) {
		source = context->handle;
	}

	// 目的地址为 0, 将不会发送信息
	if (destination == 0) {
		return session;
	}

	// 远程节点则使用 harbor 发送
	if (skynet_harbor_message_isremote(destination)) {
		// 这里分配了新的内存空间
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		
		// 数据会在 service_harbor.c 的 mainloop 函数里面将这个数据释放掉.
		rmsg->message = data;
		rmsg->sz = sz;
		skynet_harbor_send(rmsg, source, session);
	} else {
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;

		// 数据在 dispatch_message 中被删除掉
		smsg.data = data;
		smsg.sz = sz;

		// 压入到目标 skynet_context 的队列中
		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr, int type, int session, void * data, size_t sz) {
	// 数据大小的判断
	if (source == 0) {
		source = context->handle;
	}

	// 拿到目标地址的值
	uint32_t des = 0;
	if (addr[0] == ':') {
		des = strtoul(addr + 1, NULL, 16);
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);
		if (des == 0) {
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
		
	// 目标地址在其他节点
	} else {
		_filter_args(context, type, &session, (void **)&data, &sz);

		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz;

		skynet_harbor_send(rmsg, source, session);
		return session;
	}

	return skynet_send(context, source, des, type, session, data, sz);
}

uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}

void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

	skynet_mq_push(ctx->queue, &smsg);
}

void 
skynet_globalinit(void) {
	G_NODE.total = 0;
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;

	// 该函数从TSD池中分配一项，将其值赋给key供以后访问使用。
	// 如果 destr_function 不为空，在线程退出（pthread_exit()）时将以key所关联的数据为参数调用 destr_function()，以释放分配的缓冲区。
	// 不论哪个线程调用 pthread_key_create()，所创建的key都是所有线程可访问的，
	// 但各个线程可根据自己的需要往key中填入不同的值，这就相当于提供了一个同名而不同值的全局变量。
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}

	// set mainthread's key
	// 设置主线程的键
	skynet_initthread(THREAD_MAIN);
}

void 
skynet_globalexit(void) {

	// 注销一个TSD采用如下API
	// 这个函数并不检查当前是否有线程正使用该TSD，也不会调用清理函数（destr_function），而只是将TSD释放以供下一次调用 pthread_key_create()使用。
	// 在LinuxThreads中，它还会将与之相关的线程数据项设为NULL.
	pthread_key_delete(G_NODE.handle_key);
}

void
skynet_initthread(int m) {

	// intptr_t/uintptr_t: 无符号与指针空间等宽度整型, 具体的了解看下面的 3 个链接
	// http://www.cnblogs.com/Anker/p/3438480.html
	// http://blog.csdn.net/menzi11/article/details/9322251
	// http://blog.csdn.net/lsjseu/article/details/42360709
	uintptr_t v = (uint32_t)(-m);

	// 为指定线程特定数据键设置线程特定绑定
	// 写入（pthread_setspecific()）时，将pointer的值（不是所指的内容）与key相关联.
	// 各个线程使用相同的 handle_key 却对应着不同的值, 达到线程存储私有化.
	// 注意: pthread_setspecific 存储的值是指针值, 但是现在将实际的数值当成指针地址.
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

