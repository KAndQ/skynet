#include "skynet.h"

#include "skynet_socket.h"
#include "socket_server.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_harbor.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static struct socket_server * SOCKET_SERVER = NULL;

void 
skynet_socket_init() {
	SOCKET_SERVER = socket_server_create();
}

void
skynet_socket_exit() {
	socket_server_exit(SOCKET_SERVER);
}

void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER);
	SOCKET_SERVER = NULL;
}

// mainloop thread
// 主循环线程

/**
 * 将 socket_message 内容转化为 skynet_socket_message, 再使用 skynet_message 保存 skynet_socket_message 的引用, 
 * 最后把 skynet_message 存入到 skynet_context 的队列中.
 * @param type 当前处理的 SOCKET 操作类型
 * @param padding true 表示 result->data 是字符串类型, 需要在 struct skynet_socket_message 之后填充内容; false 不需要填充
 * @param result 之前 socket_server 处理得到的结果
 */
static void
forward_message(int type, bool padding, struct socket_message * result) {

	// 计算需要分配的存储空间
	struct skynet_socket_message *sm;
	size_t sz = sizeof(*sm);
	if (padding) {
		if (result->data) {
			size_t msg_sz = strlen(result->data);
			if (msg_sz > 128) {
				msg_sz = 128;
			}
			sz += msg_sz;
		} else {
			result->data = "";
		}
	}

	// 将 socket_message 使用 skynet_socket_message 存储
	sm = (struct skynet_socket_message *)skynet_malloc(sz);
	sm->type = type;
	sm->id = result->id;
	sm->ud = result->ud;
	if (padding) {
		// 在 struct skynet_socket_message 的内存之后, 填充内容, 实际的 sm->buffer 指针设置为 NULL
		sm->buffer = NULL;
		memcpy(sm + 1, result->data, sz - sizeof(*sm));
	} else {
		sm->buffer = result->data;
	}

	// 将 skynet_socket_message 使用 skynet_message 保存数据
	struct skynet_message message;
	message.source = 0;
	message.session = 0;
	message.data = sm;
	message.sz = sz | ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);
	
	if (skynet_context_push((uint32_t)result->opaque, &message)) {
		// todo: report somewhere to close socket
		// 待办事项: 报告在某处关闭 socket
		// don't call skynet_socket_close here (It will block mainloop)
		// 不要在这里点用 skynet_socket_close 方法(它会阻塞主线程)
		skynet_free(sm->buffer);
		skynet_free(sm);
	}
}

int 
skynet_socket_poll() {
	struct socket_server *ss = SOCKET_SERVER;
	assert(ss);
	struct socket_message result;
	int more = 1;
	int type = socket_server_poll(ss, &result, &more);
	switch (type) {
	case SOCKET_EXIT:
		return 0;
	case SOCKET_DATA:
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result);
		break;
	case SOCKET_CLOSE:
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;
	case SOCKET_OPEN:
		// 在主动去连接其他 socket 的时候, result->data 存储的字符串数据是连接方的地址, socket_server.buffer 的数据.;
		// bind 的时候存储的是 "binding";
		// socket 从 PACCEPT -> CONNECTED 或者 socket 从 PLISTEN -> LISTEN 时存储的是 "start";
		// 当 socket 是 CONNECTED 状态时, 存储的是 "transfer".
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;
	case SOCKET_ERROR:
		forward_message(SKYNET_SOCKET_TYPE_ERROR, true, &result);
		break;
	case SOCKET_ACCEPT:
		// 当有其他 socket 连接到侦听的 socket, result->data 以 "xxx.xxx.xxx.xxx:端口号" 的格式保存连接方的地址信息, socket_server.buffer 的数据.
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
		break;
	case SOCKET_UDP:
		forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
		break;
	default:
		skynet_error(NULL, "Unknown socket message type %d.",type);
		return -1;
	}

	if (more) {
		return -1;
	}

	return 1;
}

/// 检测/警告 id 对应的 socket 有多少数据没有被发送
static int
check_wsz(struct skynet_context *ctx, int id, void *buffer, int64_t wsz) {
	if (wsz < 0) {
		return -1;
	} else if (wsz > 1024 * 1024) {
		struct skynet_socket_message tmp;
		tmp.type = SKYNET_SOCKET_TYPE_WARNING;
		tmp.id = id;
		tmp.ud = (int)(wsz / 1024);
		tmp.buffer = NULL;
		skynet_send(ctx, 0, skynet_context_handle(ctx), PTYPE_SOCKET, 0 , &tmp, sizeof(tmp));	// 注意, 这里会复制 skynet_socket_message 的数据
//		skynet_error(ctx, "%d Mb bytes on socket %d need to send out", (int)(wsz / (1024 * 1024)), id);
	}
	return 0;
}

int
skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz) {
	int64_t wsz = socket_server_send(SOCKET_SERVER, id, buffer, sz);
	return check_wsz(ctx, id, buffer, wsz);
}

void
skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz) {
	socket_server_send_lowpriority(SOCKET_SERVER, id, buffer, sz);
}

int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}

int 
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_bind(SOCKET_SERVER, source, fd);
}

void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}

void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}

void
skynet_socket_nodelay(struct skynet_context *ctx, int id) {
	socket_server_nodelay(SOCKET_SERVER, id);
}

int 
skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp(SOCKET_SERVER, source, addr, port);
}

int 
skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port) {
	return socket_server_udp_connect(SOCKET_SERVER, id, addr, port);
}

int 
skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz) {
	int64_t wsz = socket_server_udp_send(SOCKET_SERVER, id, (const struct socket_udp_address *)address, buffer, sz);
	return check_wsz(ctx, id, (void *)buffer, wsz);
}

const char *
skynet_socket_udp_address(struct skynet_socket_message *msg, int *addrsz) {
	if (msg->type != SKYNET_SOCKET_TYPE_UDP) {
		return NULL;
	}
	struct socket_message sm;
	sm.id = msg->id;
	sm.opaque = 0;
	sm.ud = msg->ud;
	sm.data = msg->buffer;
	return (const char *)socket_server_udp_address(SOCKET_SERVER, &sm, addrsz);
}
