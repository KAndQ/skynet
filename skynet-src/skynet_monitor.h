/**
 * monitor 的主要功能其实当消息被 dispatch 的时候, 定制了一个标记, 判断是否进入了死循环.
 * 实现的逻辑其实是开了一个线程, 可以查看 skynet_start.c 的 thread_monitor, 这个就是 monitor 专用的线程.
 */

#ifndef SKYNET_MONITOR_H
#define SKYNET_MONITOR_H

#include <stdint.h>

struct skynet_monitor;

// 创建 skynet_monitor
struct skynet_monitor * skynet_monitor_new();

// 删除掉 skynet_monitor
void skynet_monitor_delete(struct skynet_monitor *);

// 校验前初始化, 防止 dispatch_message 在派发消息的时候进入死循环(或者执行的时间太久, 做一个警报)
void skynet_monitor_trigger(struct skynet_monitor *, uint32_t source, uint32_t destination);

// 可以理解为警报触发校验, 注意, 它和 trigger 不在同一线程
void skynet_monitor_check(struct skynet_monitor *);

#endif
