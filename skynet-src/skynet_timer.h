#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

int skynet_timeout(uint32_t handle, int time, int session);

/**
 * thread_timer 线程循环运行的时间更新函数, 可以在 skynet_start.c 文件中查看
 */
void skynet_updatetime(void);
uint32_t skynet_gettime(void);
uint32_t skynet_gettime_fixsec(void);

void skynet_timer_init(void);

#endif
