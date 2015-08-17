#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

// 关联的并且跟远程节点通信的 skynet_context
static struct skynet_context * REMOTE = 0;

// 当前节点的 harbor, 使用高 8 位存储当前的 harbor 数据
static unsigned int HARBOR = ~0;	// 0xffffffff

void 
skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session) {
	// 获得当前发送消息的类型, 高 8 位存的是 PTEXT_*, 请查看 skynet.h
	int type = rmsg->sz >> MESSAGE_TYPE_SHIFT;

	// 截取实际的发送数据大小
	rmsg->sz &= MESSAGE_TYPE_MASK;

	// type 校验
	assert(type != PTYPE_SYSTEM && type != PTYPE_HARBOR && REMOTE);

	// 使用 REMOTE 发送数据给目标节点
	skynet_context_send(REMOTE, rmsg, sizeof(*rmsg), source, type, session);
}

int 
skynet_harbor_message_isremote(uint32_t handle) {
	// 保证初始化过
	assert(HARBOR != ~0);

	// 获得 handle 的 harbor, 只取高 8 位的数据
	int h = (handle & ~HANDLE_MASK);

	// handle 的 harbor 与本节点的 harbor 做比较
	return h != HARBOR && h != 0;
}

void
skynet_harbor_init(int harbor) {
	// 初始化当前节点的 harbor
	HARBOR = (unsigned int)harbor << HANDLE_REMOTE_SHIFT;
}

void
skynet_harbor_start(void *ctx) {
	// the HARBOR must be reserved to ensure the pointer is valid.
	// It will be released at last by calling skynet_harbor_exit
	// HARBOR 必须被保留引用, 确认指针是可用的
	// 在调用 skynet_harbor_exit 之后将被删除掉
	// 注意这里调用的是 skynet_context_reserve 说明这是整个 skynet 系统保留的, 不在 G_NODE 的统计里面
	skynet_context_reserve(ctx);
	REMOTE = ctx;
}

void
skynet_harbor_exit() {
	// 确保其他线程使用不到
	struct skynet_context * ctx = REMOTE;
	REMOTE = NULL;

	// 释放引用
	if (ctx) {
		skynet_context_release(ctx);
	}
}
