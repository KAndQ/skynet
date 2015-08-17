#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"
#include "atomic.h"

#include <stdlib.h>
#include <string.h>

struct skynet_monitor {
	int version;				// 当前 version
	int check_version;			// 校验 version, 当 check_version 和 version 相同的时候是非法情况
	uint32_t source;			// 发送源
	uint32_t destination;		// 目标源
};

struct skynet_monitor * 
skynet_monitor_new() {
	// 分配内存资源, 并初始化
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));
	memset(ret, 0, sizeof(*ret));
	return ret;
}

void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	// 释放内存资源
	skynet_free(sm);
}

void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	// 在发送消息前, 记录一次状态
	sm->source = source;
	sm->destination = destination;

	ATOM_INC(&sm->version);
}

// check 函数和上面的 trigger 函数不是运行在同一个线程的
void 
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {
		if (sm->destination) {		// skynet_context 在 dispatch_message 数据之后, 会设置 destination 参数为 0.
			skynet_context_endless(sm->destination);
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source, sm->destination, sm->version);
		}
	} else {
		sm->check_version = sm->version;
	}
}