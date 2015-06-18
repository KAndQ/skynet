#ifndef SKYNET_MONITOR_H
#define SKYNET_MONITOR_H

#include <stdint.h>

struct skynet_monitor;

// 创建 skynet_monitor
struct skynet_monitor * skynet_monitor_new();

// 删除掉 skynet_monitor
void skynet_monitor_delete(struct skynet_monitor *);

// 校验前初始化, 防止消息传递的无限循环
void skynet_monitor_trigger(struct skynet_monitor *, uint32_t source, uint32_t destination);

// 校验
void skynet_monitor_check(struct skynet_monitor *);

#endif
