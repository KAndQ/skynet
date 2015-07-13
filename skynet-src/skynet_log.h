#ifndef skynet_log_h
#define skynet_log_h

#include "skynet_env.h"
#include "skynet.h"

#include <stdio.h>
#include <stdint.h>

/**
 * 为一个服务开启日志, 日志文件名以 handle 的16进制表示
 * @param ctx skynet_error 发送消息的 ctx
 * @param handle 处理日志的服务 handle
 * @return 返回创建/打开的文件句柄
 */
FILE * skynet_log_open(struct skynet_context * ctx, uint32_t handle);

/**
 * 关闭打开的日志文件
 * @param ctx skynet_error 发送消息的 ctx
 * @param f skynet_log_open 返回的文件句柄
 * @param handle 关闭日志文件的服务 handle
 */
void skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle);

/**
 * 将信息输入到文件上
 * @param f 被输入信息的文件句柄
 * @param source 消息源
 * @param type 消息类型, 具体查看 skynet.h
 * @param session 回话 session 计数
 * @param buffer 数据指针
 * @param sz 数据大小
 */
void skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz);

#endif