/**
 * 将 skynet 与 socket_server 结合起来.
 */

#ifndef skynet_socket_h
#define skynet_socket_h

struct skynet_context;

#define SKYNET_SOCKET_TYPE_DATA 1       // tcp 接收到数据
#define SKYNET_SOCKET_TYPE_CONNECT 2    // 当前 skynet 节点连接到其他主机
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

int skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz);
void skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz);
int skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog);
int skynet_socket_connect(struct skynet_context *ctx, const char *host, int port);
int skynet_socket_bind(struct skynet_context *ctx, int fd);
void skynet_socket_close(struct skynet_context *ctx, int id);
void skynet_socket_start(struct skynet_context *ctx, int id);
void skynet_socket_nodelay(struct skynet_context *ctx, int id);

int skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port);
int skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port);
int skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz);
const char * skynet_socket_udp_address(struct skynet_socket_message *, int *addrsz);

#endif
