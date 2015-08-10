/**
 * 将 skynet 与 socket_server 结合起来.
 */

#ifndef skynet_socket_h
#define skynet_socket_h

struct skynet_context;

#define SKYNET_SOCKET_TYPE_DATA 1       // tcp 接收到数据
#define SKYNET_SOCKET_TYPE_CONNECT 2    // 与其他主机成功建立连接, 这时可以操作该 socket
#define SKYNET_SOCKET_TYPE_CLOSE 3      // 关闭当前 socket 
#define SKYNET_SOCKET_TYPE_ACCEPT 4     // 当前节点接收到新的连接
#define SKYNET_SOCKET_TYPE_ERROR 5      // socket 出错, 已经无法使用
#define SKYNET_SOCKET_TYPE_UDP 6        // udp 接收到数据

/// skynet 与 socket_server 的数据转化, 一般是将 socket_message 的内容传给 skynet_socket_message
struct skynet_socket_message {
	int type;  // 以上宏定义的类型
	int id;    // socket id
	int ud;    // 数据长度
	char * buffer; // 数据指针
};

/// 当前节点的 socket 环境初始化
void skynet_socket_init();

/// 请求退出当前节点的通信线程
void skynet_socket_exit();

/// 释放当前节点的 socket 环境资源
void skynet_socket_free();

/// 通信线程的逻辑处理. 返回值, 0 表示退出该线程, 1 是表示需要处理条件信号, -1 表示通信线程不需要处理条件信号
int skynet_socket_poll();

/// 基于 tcp 协议, 使用高优先级发送数据. 返回值, 发送成功返回 0, 否则返回 -1
int skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz);

/// 基于 tcp 协议, 使用低优先级发送数据. 
void skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz);

/// 侦听指定的地址端口. 返回值, 成功返回 socket id, 否则返回 -1
int skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog);

/// 连接到指定的主机. 返回值, 成功返回 socket id, 否则返回 -1
int skynet_socket_connect(struct skynet_context *ctx, const char *host, int port);

/// 绑定指定的 socket fd. 返回值, 成功返回 socket id, 否则返回 -1
int skynet_socket_bind(struct skynet_context *ctx, int fd);

/// 关闭 socket
void skynet_socket_close(struct skynet_context *ctx, int id);

/// 开启指定的 socket
void skynet_socket_start(struct skynet_context *ctx, int id);

/// 设置 socket 的 nodelay, 禁用 nagle 算法
void skynet_socket_nodelay(struct skynet_context *ctx, int id);

/// 创建爱你一个 udp socket. 返回值, 创建成功返回 socket id, 否则返回 -1
int skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port);

/// 设置指定 socket 的地址信息. 返回值, 成功返回 0, 否则返回 -1
int skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port);

/// 基于 udp 协议, 发送数据. 返回值, 发送成功返回 0, 否则返回 -1
int skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz);

/// 基于 udp 协议, 拿到 skynet_socket_message 中地址数据指针. addrsz 返回的地址数据的内存大小
const char * skynet_socket_udp_address(struct skynet_socket_message *, int *addrsz);

#endif
