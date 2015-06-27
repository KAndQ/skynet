/**
 * 守护线程的相关逻辑
 */

#ifndef skynet_daemon_h
#define skynet_daemon_h

/**
 * 守护进程初始化
 * @param pidfile 配置守护进程的文件
 * @return 初始化成功返回 0, 否则返回 1
 */
int daemon_init(const char *pidfile);

/**
 * 守护进程退出
 * @param pidfile 配置守护进程的文件
 * @return 成功返回 0, 否则返回 -1
 */
int daemon_exit(const char *pidfile);

#endif
