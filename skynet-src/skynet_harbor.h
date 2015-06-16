#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>
#include <stdlib.h>

// 在整个 skynet 网络中节点名字的长度
#define GLOBALNAME_LENGTH 16

// 整个 skynet 网络节点的数量
#define REMOTE_MAX 256

// reserve high 8 bits for remote id
// 保留高 8 位作为远程节点的 id
#define HANDLE_MASK 0xffffff        // 用于计算 handle 的值

// 计算 handle 时的位偏移量
#define HANDLE_REMOTE_SHIFT 24

// 远程节点的数据结构
struct remote_name {
	char name[GLOBALNAME_LENGTH];  // skynet 节点的名字
	uint32_t handle;               // handle 
};

// 发送给远程节点的消息
struct remote_message {
	struct remote_name destination;    // 发送的目标节点
	const void * message;              // 发送数据
	size_t sz;                         // 数据长度
};

void skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session);
int skynet_harbor_message_isremote(uint32_t handle);
void skynet_harbor_init(int harbor);
void skynet_harbor_start(void * ctx);
void skynet_harbor_exit();

#endif
