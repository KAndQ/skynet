// 网关服务器的实现, 客户端与连接这个服务连接, 这个服务接收到数据之后, 伪造 client 给 agent 发送接收到的数据, 由 agent 处理实际的逻辑.
// 这个服务实际上只是做数据的转发与 socket 的连接管理.
// 这个服务貌似目前已经不使用了, 而是直接使用 gateserver.lua + gate.lua + watchdog.lua 来代替.

#include "skynet.h"
#include "skynet_socket.h"
#include "databuffer.h"
#include "hashid.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#define BACKLOG 32

struct connection {
	int id;	// skynet_socket id
	uint32_t agent;					// skynet_context handle, 负责逻辑处理的服务
	uint32_t client;				// skynet_context handle
	char remote_name[32];			// 连接客户端的主机地址
	struct databuffer buffer;		// 接收数据
};

struct gate {
	struct skynet_context *ctx;		// skynet_context
	int listen_id;					// 侦听连接的 socket id
	uint32_t watchdog;				// skynet_context handle
	uint32_t broker;				// skynet_context handle
	int client_tag;					// PTYPE_CLIENT
	int header_size;				// 数据包包头的长度
	int max_connection;				// 最大的连接数量
	struct hashid hash;				// hash 表, 存储的 id 就是 socket id, 而返回的值用于 connection 数组的索引
	struct connection *conn;		// 连接到当前主机的客户端
	// todo: save message pool ptr for release
	struct messagepool mp;			// 为 connection 的 databuffer 分配内存资源的 messagepool
};

/// 创建 struct gate 对象
struct gate *
gate_create(void) {
	struct gate * g = skynet_malloc(sizeof(*g));
	memset(g, 0, sizeof(*g));
	g->listen_id = -1;
	return g;
}

/// 删除 struct gate 对象
void
gate_release(struct gate *g) {
	int i;
	struct skynet_context *ctx = g->ctx;

	// 关闭 socket 连接
	for (i = 0; i < g->max_connection; i++) {
		struct connection *c = &g->conn[i];
		if (c->id >= 0) {
			skynet_socket_close(ctx, c->id);
		}
	}

	// 关闭侦听的 socket
	if (g->listen_id >= 0) {
		skynet_socket_close(ctx, g->listen_id);
	}

	// 释放 messagepool 资源
	messagepool_free(&g->mp);

	// 释放 hash 表资源
	hashid_clear(&g->hash);

	// 释放 struct connection 资源
	skynet_free(g->conn);

	// 释放 struct gate 资源
	skynet_free(g);
}

/**
 * 去掉 msg 字符串的命令及其之后连续的空格. 例如: msg = "kill    10000"; 处理后之后为 msg = "10000"
 * @param msg 待修改的字符串
 * @param sz 字符串长度
 * @param command_sz 命令字符串的长度
 */
static void
_parm(char *msg, int sz, int command_sz) {
	// 查询到之后的第一个非空格字符
	while (command_sz < sz) {
		if (msg[command_sz] != ' ')
			break;
		++command_sz;
	}

	// 去掉命令字符串与之后的连续空格, 将以参数开始的字符串前移
	int i;
	for (i=command_sz;i<sz;i++) {
		msg[i-command_sz] = msg[i];
	}
	msg[i-command_sz] = '\0';
}

/// 通过 socket id 查到 connection, 给 connection 的 agent 和 client 赋值
static void
_forward_agent(struct gate * g, int fd, uint32_t agentaddr, uint32_t clientaddr) {
	int id = hashid_lookup(&g->hash, fd);	// 得到 hash 键
	if (id >= 0) {
		struct connection * agent = &g->conn[id];
		agent->agent = agentaddr;		// 记录 agent 地址
		agent->client = clientaddr;		// 记录 client 地址
	}
}

/// 命令处理, 解析字符串, 然后做对应的处理
static void
_ctrl(struct gate * g, const void * msg, int sz) {
	struct skynet_context * ctx = g->ctx;

	// 复制 msg 内容
	char tmp[sz + 1];
	memcpy(tmp, msg, sz);
	tmp[sz] = '\0';

	char * command = tmp;
	int i;
	if (sz == 0)
		return;

	// 查询第一空格字符出现的索引
	for (i = 0; i < sz; i++) {
		if (command[i] == ' ') {
			break;
		}
	}

	if (memcmp(command, "kick", i) == 0) {		// 关闭掉某个建立的连接 socket
		_parm(tmp, sz, i);
		int uid = strtol(command, NULL, 10);
		int id = hashid_lookup(&g->hash, uid);
		if (id>=0) {
			skynet_socket_close(ctx, uid);
		}
		return;
	}
	if (memcmp(command, "forward", i) == 0) {
		_parm(tmp, sz, i);
		char * client = tmp;

		// char *strsep(char **stringp, const char *delim);
		// stringp 为指向欲分割的字符串，delim 为分隔符，函数将返回分隔符前面的字符串，stringp 将指向分隔符之后的字符串.

		// 获得 socket id
		char * idstr = strsep(&client, " ");
		if (client == NULL) {
			return;
		}
		int id = strtol(idstr, NULL, 10);
		
		// 获得 agent 服务 handle
		char * agent = strsep(&client, " ");
		if (client == NULL) {
			return;
		}
		uint32_t agent_handle = strtoul(agent+1, NULL, 16);

		// 获得 client 
		uint32_t client_handle = strtoul(client+1, NULL, 16);
		_forward_agent(g, id, agent_handle, client_handle);
		return;
	}
	if (memcmp(command,"broker",i)==0) {	// 查询 broker 服务, 将查询到的服务 handle 赋予 gate.broker
		_parm(tmp, sz, i);
		g->broker = skynet_queryname(ctx, command);
		return;
	}
	if (memcmp(command,"start",i) == 0) {	// start listen socket, skynet_socket 必须要 start 之后才能使用
		_parm(tmp, sz, i);
		int uid = strtol(command , NULL, 10);
		int id = hashid_lookup(&g->hash, uid);
		if (id>=0) {
			skynet_socket_start(ctx, uid);
		}
		return;
	}
	if (memcmp(command, "close", i) == 0) {	// close listen socket.
		if (g->listen_id >= 0) {
			skynet_socket_close(ctx, g->listen_id);
			g->listen_id = -1;
		}
		return;
	}
	skynet_error(ctx, "[gate] Unkown command : %s", command);
}

/// 给 watchdog 服务发送格式化字符串
static void
_report(struct gate * g, const char * data, ...) {
	if (g->watchdog == 0) {
		return;
	}

	struct skynet_context * ctx = g->ctx;
	va_list ap;
	va_start(ap, data);
	char tmp[1024];
	int n = vsnprintf(tmp, sizeof(tmp), data, ap);
	va_end(ap);

	skynet_send(ctx, 0, g->watchdog, PTYPE_TEXT,  0, tmp, n);
}

/// 将 connection 接收的数据转发到其他服务中去
static void
_forward(struct gate *g, struct connection * c, int size) {
	struct skynet_context * ctx = g->ctx;

	// 存在 broker 服务, 将消息发给 broker 服务
	if (g->broker) {
		void * temp = skynet_malloc(size);
		databuffer_read(&c->buffer, &g->mp, temp, size);
		skynet_send(ctx, 0, g->broker, g->client_tag | PTYPE_TAG_DONTCOPY, 0, temp, size);
		return;
	}

	// 存在 agent 服务, 将消息发给 agent 服务
	if (c->agent) {
		void * temp = skynet_malloc(size);
		databuffer_read(&c->buffer, &g->mp, temp, size);

		// 伪装数据成 client 发送给 agent
		skynet_send(ctx, c->client, c->agent, g->client_tag | PTYPE_TAG_DONTCOPY, 0 , temp, size);
	} else if (g->watchdog) {	// 发送给 watchdog 服务, 但是实际数据前加入连接的 id 数据
		char * tmp = skynet_malloc(size + 32);

		// int snprintf(char *str, size_t size, const char *format, ...)
		// 若成功则返回欲写入的字符串长度，若出错则返回负值。
		int n = snprintf(tmp, 32, "%d data ", c->id);
		databuffer_read(&c->buffer, &g->mp, tmp + n, size);	// 将实际数据读入到 tmp+n 的位置
		skynet_send(ctx, 0, g->watchdog, PTYPE_TEXT | PTYPE_TAG_DONTCOPY, 0, tmp, size + n);
	}
}

/// 对接收的消息做存储转发处理
static void
dispatch_message(struct gate *g, struct connection *c, int id, void * data, int sz) {
	// 将接收的数据压入到 databuffer 中
	databuffer_push(&c->buffer, &g->mp, data, sz);
	for (;;) {
		// 读取有效的数据头
		int size = databuffer_readheader(&c->buffer, &g->mp, g->header_size);
		if (size < 0) {
			return;
		} else if (size > 0) {
			if (size >= 0x1000000) {	// 接收数据大于 16M 则关闭该连接
				struct skynet_context * ctx = g->ctx;
				databuffer_clear(&c->buffer,&g->mp);
				skynet_socket_close(ctx, id);
				skynet_error(ctx, "Recv socket message > 16M");
				return;
			} else {
				_forward(g, c, size);
				databuffer_reset(&c->buffer);
			}
		}
	}
}

/// 处理 PTYPE_SOCKET 消息
static void
dispatch_socket_message(struct gate *g, const struct skynet_socket_message * message, int sz) {
	struct skynet_context * ctx = g->ctx;
	switch(message->type) {
	case SKYNET_SOCKET_TYPE_DATA: {
		int id = hashid_lookup(&g->hash, message->id);	// 查询 hash 键
		if (id >= 0) {
			struct connection *c = &g->conn[id];
			dispatch_message(g, c, message->id, message->buffer, message->ud);
		} else {
			skynet_error(ctx, "Drop unknown connection %d message", message->id);
			skynet_socket_close(ctx, message->id);
			skynet_free(message->buffer);
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_CONNECT: {
		if (message->id == g->listen_id) {
			// start listening
			// 开始侦听
			break;
		}

		int id = hashid_lookup(&g->hash, message->id);
		if (id<0) {
			skynet_error(ctx, "Close unknown connection %d", message->id);
			skynet_socket_close(ctx, message->id);
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_CLOSE:
	case SKYNET_SOCKET_TYPE_ERROR: {
		int id = hashid_remove(&g->hash, message->id);	// 读取 hash 键
		if (id >= 0) {
			struct connection *c = &g->conn[id];
			databuffer_clear(&c->buffer, &g->mp);	// 释放内存资源
			memset(c, 0, sizeof(*c));				// 重置 connection 数据
			c->id = -1;
			_report(g, "%d close", message->id);
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_ACCEPT:	// 新的客户端接入
		// report accept, then it will be get a SKYNET_SOCKET_TYPE_CONNECT message
		// 脑高 accept, 然后当前函数将获得 1 个 SKYNET_SOCKET_TYPE_CONNECT 消息
		assert(g->listen_id == message->id);
		if (hashid_full(&g->hash)) {
			skynet_socket_close(ctx, message->ud);
		} else {
			// 创建 connection 对象
			struct connection *c = &g->conn[hashid_insert(&g->hash, message->ud)];

			// 复制连接主机的地址信息
			if (sz >= sizeof(c->remote_name)) {
				sz = sizeof(c->remote_name) - 1;
			}
			// 记录连接的 socket id
			c->id = message->ud;
			memcpy(c->remote_name, message+1, sz);
			c->remote_name[sz] = '\0';
			_report(g, "%d open %d %s:0",c->id, c->id, c->remote_name);
			skynet_error(ctx, "socket open: %x", c->id);
		}
		break;
	case SKYNET_SOCKET_TYPE_WARNING:
		skynet_error(ctx, "fd (%d) send buffer (%d)K", message->id, message->ud);
		break;
	}
}

/// 每次接收到 skynet_message 的逻辑处理
static int
_cb(struct skynet_context * ctx, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct gate *g = ud;
	switch(type) {
	case PTYPE_TEXT:
		_ctrl(g, msg, (int)sz);
		break;
	case PTYPE_CLIENT: {	// 如果其他的服务回的是 PTYPE_CLIENT 类型的消息, 表示 service_gate 要把消息返回给连接的终端
		if (sz <= 4) {
			skynet_error(ctx, "Invalid client message from %x", source);
			break;
		}

		// The last 4 bytes in msg are the id of socket, write following bytes to it
		// 在 msg 里的最后 4 个字节是 socket id, 写在下面的字节中
		const uint8_t * idbuf = msg + sz - 4;
		uint32_t uid = idbuf[0] | idbuf[1] << 8 | idbuf[2] << 16 | idbuf[3] << 24;
		int id = hashid_lookup(&g->hash, uid);
		if (id >= 0) {
			// don't send id (last 4 bytes)
			// 不必发送 id (最后 4 个字节)
			skynet_socket_send(ctx, uid, (void*)msg, sz-4);
			// return 1 means don't free msg
			// 返回 1 表示不要释放 msg
			return 1;
		} else {
			skynet_error(ctx, "Invalid client id %d from %x",(int)uid,source);
			break;
		}
	}
	case PTYPE_SOCKET:
		// recv socket message from skynet_socket
		// 从 skynet_socket 接收 socket 消息
		dispatch_socket_message(g, msg, (int)(sz - sizeof(struct skynet_socket_message)));
		break;
	}
	return 0;
}

/// 当前机器开始侦听, 成功返回 0, 否则返回 1
static int
start_listen(struct gate * g, char * listen_addr) {
	struct skynet_context * ctx = g->ctx;

	// 读取地址和端口参数
	char * portstr = strchr(listen_addr,':');
	const char * host = "";	// 如果是空字符串传给 skynet_socket_listen, 那么侦听的地址参数默认为 0.0.0.0
	int port;
	if (portstr == NULL) {
		port = strtol(listen_addr, NULL, 10);
		if (port <= 0) {
			skynet_error(ctx, "Invalid gate address %s",listen_addr);
			return 1;
		}
	} else {
		port = strtol(portstr + 1, NULL, 10);
		if (port <= 0) {
			skynet_error(ctx, "Invalid gate address %s",listen_addr);
			return 1;
		}
		portstr[0] = '\0';
		host = listen_addr;
	}

	// 开始侦听
	g->listen_id = skynet_socket_listen(ctx, host, port, BACKLOG);
	if (g->listen_id < 0) {
		return 1;
	}
	skynet_socket_start(ctx, g->listen_id);
	return 0;
}

/// 初始化 struct gate 资源
int
gate_init(struct gate *g , struct skynet_context * ctx, char * parm) {
	if (parm == NULL)
		return 1;

	// 获得传入的参数
	int max = 0;
	int sz = strlen(parm)+1;
	char watchdog[sz];
	char binding[sz];
	int client_tag = 0;
	char header;

	int n = sscanf(parm, "%c %s %s %d %d", &header, watchdog, binding, &client_tag, &max);
	if (n<4) {
		skynet_error(ctx, "Invalid gate parm %s", parm);
		return 1;
	}

	if (max <= 0) {
		skynet_error(ctx, "Need max connection");
		return 1;
	}

	if (header != 'S' && header !='L') {
		skynet_error(ctx, "Invalid data header style");
		return 1;
	}

	if (client_tag == 0) {
		client_tag = PTYPE_CLIENT;
	}

	// 判断是否开启 watchdog
	if (watchdog[0] == '!') {
		g->watchdog = 0;
	} else {
		g->watchdog = skynet_queryname(ctx, watchdog);
		if (g->watchdog == 0) {
			skynet_error(ctx, "Invalid watchdog %s",watchdog);
			return 1;
		}
	}

	g->ctx = ctx;

	// 初始化 hash 表
	hashid_init(&g->hash, max);

	// 初始化 connection 数组
	g->conn = skynet_malloc(max * sizeof(struct connection));
	memset(g->conn, 0, max * sizeof(struct connection));
	g->max_connection = max;
	int i;
	for (i=0;i<max;i++) {
		g->conn[i].id = -1;
	}
	
	g->client_tag = client_tag;
	g->header_size = header == 'S' ? 2 : 4;	// 能够表示存储数据的总长度

	skynet_callback(ctx, g, _cb);

	return start_listen(g, binding);
}
