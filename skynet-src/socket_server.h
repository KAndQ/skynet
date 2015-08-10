/**
 * 底层的 socket 通信逻辑, 与之关联的只有 socket_pool.h
 * 可以说一个 socket 的教科书教程. 纯纯的 socket 实现, 和山楂树一样.
 */

#ifndef skynet_socket_server_h
#define skynet_socket_server_h

#include <stdint.h>

#define SOCKET_DATA 0       // tcp 协议, 接收数据成功时的返回值
#define SOCKET_CLOSE 1      // socket 已经关闭
#define SOCKET_OPEN 2       // 当前的 socket 可操作
#define SOCKET_ACCEPT 3     // 接入新的 socket 连接
#define SOCKET_ERROR 4      // socket 操作产生错误, 这时的 socket 是无法操作的
#define SOCKET_EXIT 5       // 当前 skynet 节点退出通信的轮询
#define SOCKET_UDP 6        // udp 协议, 接收数据成功时的返回值

struct socket_server;

/// 主要用于在操作 socket 时, 存储的操作 socket 的相关信息
struct socket_message {
	int id;            // socket id
	uintptr_t opaque;  // 不透明的功能作用, 目前在 skynet_socket 中当作 skynet_context 的 handle 使用

    // for accept, ud is listen id; for data, ud is size of data.
    // 对于 accpet, ud 是侦听的 id; 对于 data, ud 是数据的大小
	int ud;
	char * data;
};

/// 创建 socket_server 对象
struct socket_server * socket_server_create();

/// 释放 socket_server 对象资源
void socket_server_release(struct socket_server *);

/**
 * 轮询的方式从 event pool 中取出 event, 然后对可操作的 socket 做操作, 使用 socket 发送/接收数据等等.
 * 还有一些 socket 的内部逻辑操作.
 * @param socket_server
 * @param socket_message 操作结果
 * @param more 每次当 event pool 事件处理完重置的时候, 会将 *more 的值设置为 0
 * @return 上面定义的宏 SOCKET_XXX 这些
 */
int socket_server_poll(struct socket_server *, struct socket_message *result, int *more);

/// 请求退出
void socket_server_exit(struct socket_server *);

/// 请求关闭指定的 socket
void socket_server_close(struct socket_server *, uintptr_t opaque, int id);

/// 请求打开一个 socket, socket 在 start 之后才能被操作
void socket_server_start(struct socket_server *, uintptr_t opaque, int id);

// return -1 when error
// 错误时返回 -1

/// 请求使用 [高] 优先级队列发送数据. 成功返回当前待发送数据的大小, 否则返回 -1
int64_t socket_server_send(struct socket_server *, int id, const void * buffer, int sz);

/// 请求使用 [低] 优先级队列发送数据. 成功返回当前待发送数据的大小, 否则返回 -1
void socket_server_send_lowpriority(struct socket_server *, int id, const void * buffer, int sz);

// ctrl command below returns id
// 控制命令在返回的 id 掩饰之下
/// 请求开始侦听指定的 [地址, 端口], 返回值, 成功返回 socket id, 否则返回 -1
/// 这个函数首先会 bind, 然后再 listen
int socket_server_listen(struct socket_server *, uintptr_t opaque, const char * addr, int port, int backlog);

/// 请求连接到指定的主机. 返回值, 如果请求成功返回 socket id, 否则返回 -1
int socket_server_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);

/// 请求将指定的 fd 设置为 bind 状态
int socket_server_bind(struct socket_server *, uintptr_t opaque, int fd);

// for tcp
// 适用于 tcp
/// 请求设置 id 对应的 tcp 禁用 nagle 算法
void socket_server_nodelay(struct socket_server *, int id);

struct socket_udp_address;

// create an udp socket handle, attach opaque with it . udp socket don't need call socket_server_start to recv message
// if port != 0, bind the socket . if addr == NULL, bind ipv4 0.0.0.0 . If you want to use ipv6, addr can be "::" and port 0.
// 创建一个 udp socket 句柄, 关联 opaque. udp socket 不需要调用 socket_server_start 来接收信息.
// 如果 port != 0, 绑定一个 socket. 如果 addr == NULL, 绑定 ipv4 的 0.0.0.0 地址. 如果你希望使用 ipv6, addr = "::" 同时 port = 0.
/// 请求创建一个 udp socket. 返回值, 成功返回 socket id, 否则返回 -1
int socket_server_udp(struct socket_server *, uintptr_t opaque, const char * addr, int port);

// set default dest address, return 0 when success
// 设置默认的目标地址, 成功返回 0, 失败返回 -1
int socket_server_udp_connect(struct socket_server *, int id, const char * addr, int port);

// If the socket_udp_address is NULL, use last call socket_server_udp_connect address instead
// You can also use socket_server_send 
// 如果 socket_udp_address 为 NULL, 使用上次调用 socket_server_udp_connect 函数时使用的地址代替
// 你也能够使用 socket_server_send 函数
/// 请求发送 udp 数据. 返回值, 成功返回当前待发送数据的大小(未加入计算此次调用), 否则返回 -1
int64_t socket_server_udp_send(struct socket_server *, int id, const struct socket_udp_address *, const void *buffer, int sz);

// extract the address of the message, struct socket_message * should be SOCKET_UDP
// 提取地址信息, struct socket_message * 应该是 SOCKET_UDP 的返回结果
/**
 * 提取地址信息
 * @param socket_server 
 * @param socket_message SOCKET_UDP 操作的结果
 * @param addrsz 地址信息的内容大小
 * @return 地址信息的起始地址
 */
const struct socket_udp_address * socket_server_udp_address(struct socket_server *, struct socket_message *, int *addrsz);

// socket 对象的操作接口
struct socket_object_interface {
	void * (*buffer)(void *);      // 获得发送数据起始地址函数接口, 读了 send_socket 这个函数, 实现 buffer 接口的函数不应该分配新的内存空间, 否则会发生内存泄漏
	int (*size)(void *);           // 获得发送数据大小的函数接口
	void (*free)(void *);          // 释放数据资源的函数接口, 保证能够释放掉已经分配的内存空间
};

// if you send package sz == -1, use soi.
// 如果发送数据包的 sz 参数为 -1 时, 使用 soi.
// 设置 socket_server.soi
void socket_server_userobject(struct socket_server *, struct socket_object_interface *soi);

#endif
