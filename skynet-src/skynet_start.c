#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct monitor {
	int count;	// strcut skynet_monitor * 数组的大小
	struct skynet_monitor ** m;		// struct skynet_monitor * 的数组

	// pthread_conf_t 是一个结构
	// 创建条件变量, 静态方式和动态方式
	// 静态方式: 可以把常量 PTHREAD_COND_INITIALIZER 赋给静态分配的条件变量
	// 动态方式: int pthread_cond_init(pthread_cond_t * restrict cond, const pthread_condattr_t * restrict attr);
	// pthread_cond_destroy 对条件变量进行反初始化, int pthread_cond_destroy(pthread_cond_t * cond);
	// 只有在没有线程在该条件变量上等待的时候才能注销这个条件变量，否则返回EBUSY。
	pthread_cond_t cond;	// 线程条件变量

	// pthread_mutex_t 是一个结构
	// 创建互斥锁，静态方式和动态方式
	// 静态方式: POSIX定义了一个宏 PTHREAD_MUTEX_INITIALIZER 来静态初始化互斥锁, 而 PTHREAD_MUTEX_INITIALIZER则是一个结构常量。
	// 动态方式: int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr)
	// mutexattr: 用于指定互斥锁属性（见下），如果为NULL则使用缺省属性。
	// 		PTHREAD_MUTEX_TIMED_NP: 这是缺省值，也就是普通锁。当一个线程加锁以后，其余请求锁的线程将形成一个等待队列，并在解锁后按优先级获得锁。这种锁策略保证了资源分配的公平性。
	// 		PTHREAD_MUTEX_RECURSIVE_NP: 嵌套锁，允许同一个线程对同一个锁成功获得多次，并通过多次unlock解锁。如果是不同线程请求，则在加锁线程解锁时重新竞争。
	// 		PTHREAD_MUTEX_ERRORCHECK_NP: 检错锁，如果同一个线程请求同一个锁，则返回EDEADLK，否则与PTHREAD_MUTEX_TIMED_NP类型动作相同。这样就保证当不允许多次加锁时不会出现最简单情况下的死锁。
	// 		PTHREAD_MUTEX_ADAPTIVE_NP: 适应锁，动作最简单的锁类型，仅等待解锁后重新竞争。
	// pthread_mutex_destroy 用于注销一个互斥锁，int pthread_mutex_destroy(pthread_mutex_t *mutex), 销毁一个互斥锁即意味着释放它所占用的资源，且要求锁当前处于开放状态。
	// 参考: http://blog.163.com/coffee_666666/blog/static/184691114201182125470/
	pthread_mutex_t mutex;	// 互斥量

	int sleep;	// 当前睡眠的线程数量
	int quit;	// woker thread 退出标记
};

/// 传递给 thread_worker 的参数
struct worker_parm {
	struct monitor *m;		// struct monitor
	int id;	// 使用的 struct skynet_monitor * 索引
	int weight;	// 分配的权重
};

/// 如果当前节点没有 skynet_context 了, 那么则让当前节点关闭
#define CHECK_ABORT if (skynet_context_total()==0) break;

/**
 * 创建线程
 * @param thread 指向线程标识的指针
 * @param start_routine 线程运行函数的起始地址
 * @param arg 运行函数的参数
 */
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	// int pthread_create(pthread_t *tidp, const pthread_attr_t *attr, (void*)(*start_rtn)(void*), void *arg);
	// Unix操作系统（Unix、Linux、Mac OS X等）的创建线程的函数。
	// tidp: 指向线程标识符的指针
	// attr: 用来设置线程属性
	// start_rtn: 线程运行函数的起始地址
	// arg: 运行函数的参数
	// 若线程创建成功，则返回0。若线程创建失败，则返回出错编号
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// pthread_cond_signal(pthread_cond_t *cond);
		// 函数的作用是发送一个信号给另外一个正在处于阻塞等待状态的线程,使其脱离阻塞状态,继续执行.
		// 如果没有线程处在阻塞等待状态, pthread_cond_signal 也会成功返回。

		// signal sleep worker, "spurious wakeup" is harmless
		// 用信号通知睡眠的 woker 线程, "假的唤醒"是无害的
		pthread_cond_signal(&m->cond);
	}
}

/// 通信处理函数, 用于通信线程, 只有 1 个, 除非是管道没有数据可读, 否则通信线程是满负荷工作, 关于管道的读取可以查看 socket_server.c 的 block_readpipe 函数.
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
		int r = skynet_socket_poll();
		
		if (r == 0)
			break;

		if (r < 0) {
			CHECK_ABORT
			continue;
		}

		wakeup(m, 0);
	}
	return NULL;
}

/// 释放 struct monitor 资源
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

/// 监控处理函数, 用于监控线程, 只有 1 个.
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
	for (;;) {
		CHECK_ABORT
		
		// 监控检测
		for (i = 0; i < n; i++) {
			skynet_monitor_check(m->m[i]);
		}

		// idle
		for (i = 0; i < 5; i++) {
			CHECK_ABORT
			sleep(1);	// 1s
		}
	}

	return NULL;
}

/// 计时器处理函数, 用于计时器线程, 只有 1 个.
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		
		CHECK_ABORT

		wakeup(m, m->count-1);
		
		// usleep功能把进程挂起一段时间，单位是微秒（百万分之一秒）；
		// 头文件：unistd.h 
		// 语法: void usleep(int micro_seconds);
		// 返回值: 无
		// 内容说明：本函数可暂时使程序停止执行。
		// 停止 2.5 毫秒
		usleep(2500);
	}

	// wakeup socket thread
	// 唤醒 socket 线程
	skynet_socket_exit();

	// wakeup all worker thread
	// 唤醒全部的工作线程

	// int pthread_mutex_lock(pthread_mutex_t *mutex);
	// 在成功完成之后会返回零。其他任何返回值都表示出现了错误。如果出现以下任一情况，该函数将失败并返回对应的值。
	// pthread_mutex_trylock 语义与 pthread_mutex_lock 类似，不同的是在锁已经被占据时返回EBUSY而不是挂起等待。
	pthread_mutex_lock(&m->mutex);

	m->quit = 1;

	// 激活所有等待线程
	pthread_cond_broadcast(&m->cond);

	// int pthread_mutex_unlock(pthread_mutex_t *mutex);
	// 与pthread_mutex_lock成对存在。
	pthread_mutex_unlock(&m->mutex);

	return NULL;
}

/// 主要逻辑工作函数, skynet_context 的逻辑运行就在这, 可能会用于多个线程, 线程的数量由配置参数的 thread 决定.
/// 如果存在数据的是偶满负荷工作, 否则阻塞, 等待唤醒.
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	while (!m->quit) {
		q = skynet_context_message_dispatch(sm, q, weight);

		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {

				// 线程睡眠计数自增
				++ m->sleep;
				
				// "spurious wakeup" is harmless,
				// "假的唤醒"是无害的
				// because skynet_context_message_dispatch() can be call at any time.
				// 因为 skyner_context_message_dispatch() 能够在任何时候被调用.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);

				// 线程睡眠计数自减
				-- m->sleep;

				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

/// 开启相关的所有线程, 参数 thread 指的是 worker 线程的数量
static void
start(int thread) {
	pthread_t pid[thread+3];

	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	// 根据 thread 数量, 创建相同数量的 skynet_monitor
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}

	// 互斥量初始化
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}

	// 条件变量初始化
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	// 创建监控线程
	create_thread(&pid[0], thread_monitor, m);

	// 创建计时器线程
	create_thread(&pid[1], thread_timer, m);

	// 创建通信线程
	create_thread(&pid[2], thread_socket, m);

	// 创建 thread 个 worker 线程

	static int weight[] = { 
		-1, -1, -1, -1, 			// 0 次
		0, 0, 0, 0,					// 不变
		1, 1, 1, 1, 1, 1, 1, 1, 	// 除 2
		2, 2, 2, 2, 2, 2, 2, 2, 	// 除 4
		3, 3, 3, 3, 3, 3, 3, 3, };	// 除 8
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight = weight[i];
		} else {
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	// 等待所有的线程执行结束, 顺序是 timer -> socket -> worker -> monitor
	for (i = 0; i < thread + 3; i++) {

		// int pthread_join(pthread_t thread, void **retval);
		// pthread_join()函数，以阻塞的方式等待thread指定的线程结束。当函数返回时，被等待线程的资源被收回。
		// 如果线程已经结束，那么该函数会立即返回。并且thread指定的线程必须是 joinable 的。
		// thread: 线程标识符，即线程ID，标识唯一线程。
		// retval: 用户定义的指针，用来存储被等待线程的返回值。
		// 返回值: 0代表成功。 失败，返回的则是错误号。
		pthread_join(pid[i], NULL);
	}

	free_monitor(m);
}

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	sscanf(cmdline, "%s %s", name, args);	// snlua bootstrap
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

void 
skynet_start(struct skynet_config * config) {
	// 守护进程
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

	// 初始化
	skynet_harbor_init(config->harbor);
	skynet_handle_init(config->harbor);
	skynet_mq_init();
	skynet_module_init(config->module_path);
	skynet_timer_init();
	skynet_socket_init();

	// 开启打印日志服务
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	bootstrap(ctx, config->bootstrap);

	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	// harbor_exit 可能会调用 socket 方法, 所以它应该在 socket_free 之前运行
	skynet_harbor_exit();
	skynet_socket_free();

	// 关闭守护进程
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
