/**
 * skynet 中使用的轮询机制定义, 在不同的操作系统, 会选择不同的轮询机制
 */

#ifndef socket_poll_h
#define socket_poll_h

#include <stdbool.h>

// 句柄
typedef int poll_fd;

struct event {
	void * s;      // 数据指针
	bool read;     // 可读标记
	bool write;    // 可写标记
};

/**
 * 判断 fd 是否无效
 * @param fd 句柄
 * @return 返回 0 表示有效, 否则表示无效
 */
static bool sp_invalid(poll_fd fd);

/**
 * 创建 poll 的句柄
 * @return 返回可用的句柄
 */
static poll_fd sp_create();

/**
 * 关闭创建的 poll
 * @param fd poll 的句柄
 */
static void sp_release(poll_fd fd);

/**
 * 将 sock fd 添加到 poll 中
 * @param fd 之前创建的 poll 句柄
 * @param sock socket 的 fd
 * @param 关联的用户数据
 * @return 成功返回 0, 失败返回 1
 */
static int sp_add(poll_fd fd, int sock, void *ud);

/**
 * 从 poll 中删除掉 sock fd
 * @param fd 之前创建的 poll 句柄
 * @param sock socket 的 fd
 */
static void sp_del(poll_fd fd, int sock);

/**
 * 控制 poll 是否侦听 sock 的可写状态
 * @param poll_fd 之前创建的 poll 句柄
 * @param sock socket fd
 * @param ud 关联的用户数据
 * @param enable 为 true 则是可写状态, 否则只是侦听可读
 */
static void sp_write(poll_fd, int sock, void *ud, bool enable);

/**
 * 等待当前产生的时间, 并将产生的事件记录下来传递给 struct event
 * @param poll_fd 之前创建的 poll 句柄
 * @param e 记录事件的 struct event
 * @param max 能够处理事件的最大数量
 * @return 发生事件的数量
 */
static int sp_wait(poll_fd, struct event *e, int max);

/**
 * 设置当前的 socket fd 为非阻塞的
 * @param sock socket 的文件描述符
 */
static void sp_nonblocking(int sock);

#ifdef __linux__
#include "socket_epoll.h"
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#include "socket_kqueue.h"
#endif

#endif
