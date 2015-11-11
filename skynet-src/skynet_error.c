#include "skynet.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_server.h"

// stdarg.h是C语言中C标准函数库的头文件，stdarg是由standard（标准） arguments（参数）简化而来，主要目的为让函数能够接收可变参数。
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 内存基础分配大小, 扩充内存的话也是按照这个大小来扩展
#define LOG_MESSAGE_SIZE 256

/**
 * 将信息传入给 logger 服务, 让其输出信息到指定的输出源
 * @param context skynet_context
 * @param msg 格式化字符串
 * @param ... 可变参数
 */
void 
skynet_error(struct skynet_context * context, const char *msg, ...) {
	static uint32_t logger = 0;

	// 找到输出源
	if (logger == 0) {
		logger = skynet_handle_findname("logger");
	}

	// 没找到的话, 就直接退出
	if (logger == 0) {
		return;
	}

	char tmp[LOG_MESSAGE_SIZE];
	char *data = NULL;

	va_list ap;

	// 格式化输出信息
	va_start(ap,msg);
	int len = vsnprintf(tmp, LOG_MESSAGE_SIZE, msg, ap);
	va_end(ap);
	if (len >=0 && len < LOG_MESSAGE_SIZE) {
		data = skynet_strdup(tmp);
	} else {
		int max_size = LOG_MESSAGE_SIZE;
		for (;;) {
			max_size *= 2;
			data = skynet_malloc(max_size);
			va_start(ap,msg);
			len = vsnprintf(data, max_size, msg, ap);
			va_end(ap);

			// 新分配的内存大小 大于 格式化后信息界面的大小, 退出循环.
			if (len < max_size) {
				break;
			}
			skynet_free(data);
		}
	}
	if (len < 0) {
		skynet_free(data);
		perror("vsnprintf error :");
		return;
	}

	// 发送 skynet_message 给 logger 服务, skynet_message 里面携带了格式化后的数据
	// 由 logger 服务决定输入
	struct skynet_message smsg;
	if (context == NULL) {
		smsg.source = 0;
	} else {
		smsg.source = skynet_context_handle(context);
	}
	smsg.session = 0;
	smsg.data = data;
	smsg.sz = len | ((size_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);
	skynet_context_push(logger, &smsg);
}

