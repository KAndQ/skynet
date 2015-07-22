/**
 * unix 系统下的轮询实现.
 */

#ifndef poll_socket_kqueue_h
#define poll_socket_kqueue_h

// Unix和Linux特有的头文件，主要定义了与网络有关的结构、变量类型、宏、函数等.
#include <netdb.h>

// 是 C 和 C++ 程序设计语言中提供对 POSIX 操作系统 API 的访问功能的头文件的名称. 
// 定义的接口通常都是大量针对系统调用的封装. 如 fork、pipe 以及各种 I/O 原语（read、write、close 等等）.
#include <unistd.h>

// fcntl.h定义了很多宏和open,fcntl函数原型
// unistd.h定义了更多的函数原型
#include <fcntl.h>

// 使用 kqueue 机制.
// kqueue 是 FreeBSD 上的一种的多路复用机制。
// 它是针对传统的 select/poll 处理大量的文件描述符性能较低效而开发出来的。
// 详细介绍: http://www.ibm.com/developerworks/cn/aix/library/1105_huangrg_kqueue/
#include <sys/event.h>

// 在应用程序源文件中包含 <sys/types.h> 以访问 _LP64 和 _ILP32 的定义。
// 此头文件还包含适当时应使用的多个基本派生类型。
// 所有这些类型在 ILP32 编译环境中保持为 32 位值，并会在 LP64 编译环境中增长为 64 位值。
#include <sys/types.h>

// socket 的相关接口
#include <sys/socket.h>

// 互联网协议族的数据类型声明和定义
#include <netinet/in.h>

// 定义了互联网的操作
#include <arpa/inet.h>

static bool 
sp_invalid(int kfd) {
	return kfd == -1;
}

static int
sp_create() {
	// 生成一个内核事件队列，返回该队列的文件描述索。其它 API 通过该描述符操作这个 kqueue。
	return kqueue();
}

static void
sp_release(int kfd) {
	// 关闭创建的队列
	close(kfd);
}

static void 
sp_del(int kfd, int sock) {
	// struct kevent { 
    //		uintptr_t ident;       /* 事件 ID */ 
    //		short     filter;       /* 事件过滤器 */ 
    //		u_short   flags;        /* 行为标识 */ 
    //		u_int     fflags;       /* 过滤器标识值 */ 
    //		intptr_t  data;         /* 过滤器数据 */ 
    //		void      *udata;       /* 应用透传数据 */ 
 	// };
 	// 
 	// ident: 事件的 id，实际应用中，一般设置为文件描述符。
 	// filter: 可以将 kqueue filter 看作事件。
 	// 内核检测 ident 上注册的 filter 的状态，状态发生了变化，就通知应用程序。
 	// kqueue 定义了较多的 filter，本文只介绍 Socket 读写相关的 filter。
 	//		EVFILT_READ: TCP 监听 socket，如果在完成的连接队列 (已收三次握手最后一个 ACK) 中有数据，此事件将被通知。
 	//						收到该通知的应用一般调用 accept()，且可通过 data 获得完成队列的节点个数。 
 	//						流或数据报 socket，当协议栈的 socket 层接收缓冲区有数据时，该事件会被通知，并且 data 被设置成可读数据的字节数。 
 	//		EVFILT_WRIT: 当 socket 层的写入缓冲区可写入时，该事件将被通知；data 指示目前缓冲区有多少字节空闲空间。
 	// flags: 
 	//		EV_ADD: 指示加入事件到 kqueue。
 	//		EV_DELETE: 指示将传入的事件从 kqueue 中移除。
 	//		EV_ENABLE: 过滤器事件可用，注册一个事件时，默认是可用的。
 	//		EV_DISABLE: 过滤器事件不可用，当内部描述可读或可写时，将不通知应用程序。
 	//		EV_ERROR: 一个输出参数，当 changelist 中对应的描述符处理出错时，将输出这个 flag。
 	//					应用程序要判断这个 flag，否则可能出现 kevent 不断地提示某个描述符出错，却没将这个描述符从 kq 中清除。
 	// fflags: 过滤器(filter)相关的一个输入输出类型标识，有时候和 data 结合使用。
 	// data: 过滤器(filter)相关的数据值，请看 EVFILT_READ 和 EVFILT_WRITE 描述。
 	// udata: 应用自定义数据，注册的时候传给 kernel，kernel 不会改变此数据，当有事件通知时，此数据会跟着返回给应用。
	struct kevent ke;

	// EV_SET(&kev, ident, filter, flags, fflags, data, udata);
	// struct kevent 的初始化的辅助操作。

	// int kevent(int kq, const struct kevent *changelist, int nchanges, struct kevent *eventlist, int nevents, const struct timespec *timeout);
	// kq: kqueue 的文件描述符。
	// changelist: 要注册 / 反注册的事件数组；
	// nchanges: changelist 的元素个数。
	// eventlist: 满足条件的通知事件数组；
	// nevents: eventlist 的元素个数。
	// timeout: 等待事件到来时的超时时间，0，立刻返回；NULL，一直等待；如果是一个具体值，等待 timespec 时间值。
	// 返回值：可用事件的个数。

	// 移除 sock 的可读通知
	EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	kevent(kfd, &ke, 1, NULL, 0, NULL);

	// 移除 sock 的可写通知
	EV_SET(&ke, sock, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	kevent(kfd, &ke, 1, NULL, 0, NULL);
}

static int 
sp_add(int kfd, int sock, void *ud) {
	struct kevent ke;

	// sock 注册可读通知
	EV_SET(&ke, sock, EVFILT_READ, EV_ADD, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1) {
		return 1;
	}

	// sock 注册可写通知
	EV_SET(&ke, sock, EVFILT_WRITE, EV_ADD, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1) {
		// 注册可写通知失败, 同时也将其从可读通知中移除
		EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		kevent(kfd, &ke, 1, NULL, 0, NULL);
		return 1;
	}

	// 禁用掉 sock 的可写通知
	EV_SET(&ke, sock, EVFILT_WRITE, EV_DISABLE, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1) {
		// 任何失败都将移除该 sock 的可读/写通知
		sp_del(kfd, sock);
		return 1;
	}
	return 0;
}

static void 
sp_write(int kfd, int sock, void *ud, bool enable) {
	struct kevent ke;

	// 设置 sock 可写通知的状态
	EV_SET(&ke, sock, EVFILT_WRITE, enable ? EV_ENABLE : EV_DISABLE, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1) {
		// todo: check error
	}
}

static int 
sp_wait(int kfd, struct event *e, int max) {
	struct kevent ev[max];

	// 拿到满足条件的事件数组
	int n = kevent(kfd, NULL, 0, ev, max, NULL);

	// 记录当前产生的事件
	int i;
	for (i = 0; i < n; i++) {
		e[i].s = ev[i].udata;	// 数据指针

		unsigned filter = ev[i].filter;
		e[i].write = (filter == EVFILT_WRITE);	// 可写标记
		e[i].read = (filter == EVFILT_READ);	// 可读标记
	}

	return n;
}

static void
sp_nonblocking(int fd) {
	// fcntl函数可以改变已打开的文件性质
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	// 设置为非阻塞的
	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
