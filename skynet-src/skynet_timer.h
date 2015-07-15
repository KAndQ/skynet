#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

/**
 * 添加计时器
 * @param handle skynet_context 的 handle
 * @param time 计时时间
 * @param session 会话 session
 * @return 如果 handle 对应的 skynet_context 不存在, 并且 time 参数是 0 的时候, 返回 -1, 否则返回 session 值
 */
int skynet_timeout(uint32_t handle, int time, int session);

/**
 * thread_timer 线程循环运行的时间更新函数, 可以在 skynet_start.c 文件中查看
 * 计时器的主要逻辑在这里运行.
 */
void skynet_updatetime(void);

/// 得到 skynet 节点从启动到目前的系统时间, 以厘秒为单位
uint32_t skynet_gettime(void);

/// 得到 skynet 节点启动的系统时间, 以秒为单位
uint32_t skynet_gettime_fixsec(void);

/// skynet 节点计时器初始化
void skynet_timer_init(void);

#endif
