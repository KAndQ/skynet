/**
 * skynet 的 harbor(节点) 的相关的逻辑.
 * 在当前节点系统会 reserve 一个 skynet_context 专门给 harbor 使用.
 */

#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>
#include <stdlib.h>

// 在整个 skynet 网络中节点名字的长度
#define GLOBALNAME_LENGTH 16

<<<<<<< HEAD
<<<<<<< HEAD
// 整个 skynet 网络节点的数量
#define REMOTE_MAX 256

// 远程节点的数据结构
=======
>>>>>>> cloudwu/master
=======
>>>>>>> cloudwu/master
struct remote_name {
	char name[GLOBALNAME_LENGTH];  // skynet 节点的名字
	uint32_t handle;               // handle 
};

// 发送给远程节点的消息
struct remote_message {
	struct remote_name destination;    // 发送的目标节点信息
	const void * message;              // 发送数据
	size_t sz;                         // 数据长度, 高 8 位存储的是数据类型
};

/**
 * 发送数据给其他 skynet 节点
 * @param rmsg remote_message
 * @param source 发送源
 * @param session 回话标识 
 */
void skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session);

/**
 * 判断 handle 是否是远程节点
 * @param handle
 * @return 远程节点返回 1, 否则返回 0
 */
int skynet_harbor_message_isremote(uint32_t handle);

/**
 * harbor 初始化
 * @param harbor 可以是 [1, 255] 间的任意整数。一个 skynet 网络最多支持 255 个节点。每个节点有必须有一个唯一的编号。
 */
void skynet_harbor_init(int harbor);

/**
 * 当前节点关联对应的 skynet_context
 * @param ctx skynet_context
 */
void skynet_harbor_start(void * ctx);

/**
 * 当前节点取消关联的 skynet_context
 */
void skynet_harbor_exit();

#endif
