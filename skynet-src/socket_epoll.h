/**
 * linux 操作系统下的轮询实现.
 * 
 * LT(level triggered) 只要某个socket处于readable/writable状态，无论什么时候进行epoll_wait都会返回该socket；
 * ET(edge triggered) 只有某个socket从unreadable变为readable或从unwritable变为writable时，epoll_wait才会返回该socket。
 */

#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

// Unix和Linux特有的头文件，主要定义了与网络有关的结构、变量类型、宏、函数等.
#include <netdb.h>

// 是 C 和 C++ 程序设计语言中提供对 POSIX 操作系统 API 的访问功能的头文件的名称. 
// 定义的接口通常都是大量针对系统调用的封装. 如 fork、pipe 以及各种 I/O 原语（read、write、close 等等）.
#include <unistd.h>

// epoll是Linux内核为处理大批量文件描述符而作了改进的poll，是Linux下多路复用IO接口select/poll的增强版本，它能显著提高程序在大量并发连接中只有少量活跃的情况下的系统CPU利用率。
// 另一点原因就是获取事件的时候，它无须遍历整个被侦听的描述符集，只要遍历那些被内核IO事件异步唤醒而加入Ready队列的描述符集合就行了。
// epoll除了提供select/poll那种IO事件的水平触发（Level Triggered）外，还提供了边缘触发（Edge Triggered），
// 这就使得用户空间程序有可能缓存IO状态，减少epoll_wait/epoll_pwait的调用，提高应用程序效率。
#include <sys/epoll.h>

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

// fcntl.h定义了很多宏和open,fcntl函数原型
// unistd.h定义了更多的函数原型
#include <fcntl.h>

static bool 
sp_invalid(int efd) {
	return efd == -1;
}

static int
sp_create() {
	// int epoll_create(int size)
	// 创建一个epoll的句柄，size用来告诉内核这个监听的数目一共有多大。
	// 需要注意的是，当创建好epoll句柄后，它就是会占用一个fd值。
	// 自从Linux 2.6.8开始，size参数被忽略，但是依然要大于0。
	return epoll_create(1024);
}

static void
sp_release(int efd) {
	// 使用完epoll后，必须调用close()关闭，否则可能导致fd被耗尽。
	close(efd);
}

static int 
sp_add(int efd, int sock, void *ud) {
	// struct epoll_event结构如下：
	// struct epoll_event {
	// 		__uint32_t events; /* Epoll events */
	// 		epoll_data_t data; /* User data variable */
	// };
	// events可以是以下几个宏的集合：
	// 		EPOLLIN ：表示对应的文件描述符可以读（包括对端SOCKET正常关闭）；
	//		EPOLLOUT：表示对应的文件描述符可以写；
	//		EPOLLPRI：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）；
	//		EPOLLERR：表示对应的文件描述符发生错误；
	//		EPOLLHUP：表示对应的文件描述符被挂断；
	//		EPOLLET： 将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)来说的。
	//				  关于 ET(Edge Triggered) 和 LT(Level Triggered) 的简单介绍看文件的最上方.
	//				  http://my.oschina.net/miffa/blog/201242
	//		EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里。
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = ud;

	// int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
	// epoll的事件注册函数，它不同与select()是在监听事件时告诉内核要监听什么类型的事件，而是在这里先注册要监听的事件类型。
	// 第一个参数是epoll_create()的返回值
	// 第二个参数表示动作，用三个宏来表示：
	//		EPOLL_CTL_ADD：注册新的fd到epfd中；
	//		EPOLL_CTL_MOD：修改已经注册的fd的监听事件；
	//		EPOLL_CTL_DEL：从epfd中删除一个fd；
	// 第三个参数是需要监听的fd
	// 第四个参数是告诉内核需要监听什么事, struct epoll_event 类型
	// 成功返回0; 失败返回-1;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

static void 
sp_del(int efd, int sock) {
	// 从 efd 的 epoll 中将 sock 移除
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}

static void 
sp_write(int efd, int sock, void *ud, bool enable) {
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}

static int 
sp_wait(int efd, struct event *e, int max) {
	struct epoll_event ev[max];

	// int epoll_wait(int epfd, struct epoll_event * events, int maxevents, int timeout);
	// 等待事件的产生
	// 参数events用来从内核得到事件的集合
	// maxevents告之内核这个events有多大，这个maxevents的值不能大于创建epoll_create()时的size
	// 参数timeout是超时时间（毫秒，0会立即返回，-1将不确定，也有说法说是永久阻塞）
	// 该函数返回需要处理的事件数目，如返回0表示已超时。
	// 
	// 工作原理: 等侍注册在epfd上的socket fd的事件的发生，如果发生则将发生的sokct fd和事件类型放入到events数组中。
	// 并且将注册在epfd上的socket fd的事件类型给清空，所以如果下一个循环你还要关注这个socket fd的话，
	// 则需要用epoll_ctl(epfd,EPOLL_CTL_MOD,listenfd,&ev)来重新设置socket fd的事件类型。
	// 这时不用EPOLL_CTL_ADD,因为socket fd并未清空，只是事件类型清空。这一步非常重要。
	int n = epoll_wait(efd, ev, max, -1);
	
	// 记录当前产生的事件, 将结果传给 struct event
	int i;
	for (i = 0; i < n; i++) {
		e[i].s = ev[i].data.ptr;	// 数据指针

		unsigned flag = ev[i].events;
		e[i].write = (flag & EPOLLOUT) != 0;	// 可写内容
		e[i].read = (flag & EPOLLIN) != 0;		// 可读标记
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

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
