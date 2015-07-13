#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

// 当前节点配置的数据结构, 由启动的 config 文件提供.
struct skynet_config {
	int thread;
	int harbor;
	const char * daemon;
	const char * module_path;
	const char * bootstrap;
	const char * logger;
	const char * logservice;
};

#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4

/**
 * 根据配置, 启动 skynet 节点
 * @param config 节点配置
 */
void skynet_start(struct skynet_config * config);

#endif
