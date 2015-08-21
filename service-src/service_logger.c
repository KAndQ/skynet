#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

struct logger {
	FILE * handle;		// 关联的文件句柄
	int close;			// 标记是否关闭了
};

/// 创建 struct logger 对象
struct logger *
logger_create(void) {
	struct logger * inst = skynet_malloc(sizeof(*inst));
	inst->handle = NULL;
	inst->close = 0;
	return inst;
}

/// 释放 struct logger 对象
void
logger_release(struct logger * inst) {
	if (inst->close) {
		fclose(inst->handle);
	}
	skynet_free(inst);
}

/// 消息处理, 打印出数据
static int
_logger(struct skynet_context * context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct logger * inst = ud;
	fprintf(inst->handle, "[:%08x] ",source);
	fwrite(msg, sz , 1, inst->handle);
	fprintf(inst->handle, "\n");
	fflush(inst->handle);

	return 0;
}

/// 初始化 struct logger
int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	// 决定消息的输出源
	if (parm) {
		inst->handle = fopen(parm,"w");
		if (inst->handle == NULL) {
			return 1;
		}
		inst->close = 1;
	} else {
		inst->handle = stdout;
	}

	if (inst->handle) {
		// 设置处理消息的函数
		skynet_callback(ctx, inst, _logger);

		// 给服务注册名字
		skynet_command(ctx, "REG", ".logger");
		return 0;
	}
	return 1;
}
