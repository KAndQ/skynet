#include "skynet_log.h"
#include "skynet_timer.h"
#include "skynet.h"
#include "skynet_socket.h"
#include <string.h>
#include <time.h>

FILE * 
skynet_log_open(struct skynet_context * ctx, uint32_t handle) {
	// 得到全局的环境变量, 可以查看 config 文件
	const char * logpath = skynet_getenv("logpath");
	if (logpath == NULL)
		return NULL;

	// 得到文件名
	size_t sz = strlen(logpath);
	char tmp[sz + 16];
	sprintf(tmp, "%s/%08x.log", logpath, handle);

	// 创建/打开日志文件
	FILE *f = fopen(tmp, "ab");
	if (f) {
		// 记录打开文件的时间, 以秒为单位
		uint32_t starttime = skynet_gettime_fixsec();
		uint32_t currenttime = skynet_gettime();
		time_t ti = starttime + currenttime / 100;
		skynet_error(ctx, "Open log file %s", tmp);
		fprintf(f, "open time: %u %s", currenttime, ctime(&ti));
		fflush(f);
	} else {
		skynet_error(ctx, "Open log file %s fail", tmp);
	}
	return f;
}

void
skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle) {
	skynet_error(ctx, "Close log file :%08x", handle);
	fprintf(f, "close time: %u\n", skynet_gettime());
	fclose(f);
}

/**
 * 将每个字节的数据以 16 进制的形式写入到文件中
 * @param f 操作文件
 * @param buffer 缓存数据
 * @param sz 数字长度
 */
static void
log_blob(FILE *f, void * buffer, size_t sz) {
	size_t i;
	uint8_t * buf = buffer;
	for (i=0; i!=sz; i++) {
		// 每个字节数据, 以 16 进制输出
		fprintf(f, "%02x", buf[i]);
	}
}

/**
 * message 的 buffer 值决定写入数据的方式
 * @param f 操作文件
 * @param message skynet_socket_message
 * @param sz 总数据大小
 */
static void
log_socket(FILE * f, struct skynet_socket_message * message, size_t sz) {
	fprintf(f, "[socket] %d %d %d ", message->type, message->id, message->ud);

	if (message->buffer == NULL) {
		// message 之后跟着的是 buffer 指针
		const char *buffer = (const char *)(message + 1);

		// 减去 message 的数据大小
		sz -= sizeof(*message);

		// 从buf所指内存区域的前 sz 个字节查找字符 '\0'
		// 当第一次遇到字符 '\0' 时停止查找。如果成功，返回指向字符 '\0' 的指针；否则返回NULL。
		const char * eol = memchr(buffer, '\0', sz);

		// 得到实际打印字符串的大小
		if (eol) {
			sz = eol - buffer;
		}

		// 输出字符串, 会在这行的最前面增加一个空格
		fprintf(f, "[%*s]", (int)sz, (const char *)buffer);
	} else {
		sz = message->ud;
		log_blob(f, message->buffer, sz);
	}
	fprintf(f, "\n");
	fflush(f);
}

void 
skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz) {
	if (type == PTYPE_SOCKET) {
		log_socket(f, buffer, sz);
	} else {
		// 将数据输入到文件上
		uint32_t ti = skynet_gettime();
		fprintf(f, ":%08x %d %d %u ", source, type, session, ti);
		log_blob(f, buffer, sz);
		fprintf(f,"\n");
		fflush(f);
	}
}
