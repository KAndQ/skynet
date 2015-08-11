#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

// 当前节点配置的数据结构, 由启动的 config 文件提供.
struct skynet_config {
	int thread;            // 开启的线程数量
	int harbor;            // 当前 skynet 节点的 id
	const char * daemon;   // 守护进程使用文件
	const char * module_path;  // c 模块的搜索路径
	const char * bootstrap;    // skynet 启动的第一个服务以及其启动参数
	const char * logger;       // skynet_error 日志输出的文件
	const char * logservice;   // 定制的 log 服务
};

// 以下是各个线程私有变量初始化时使用的值
#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4

/**
 * 根据配置, 启动 skynet 节点
 * @param config 节点配置, skynet_config 的初始化请看 skynet_main.c
 */
void skynet_start(struct skynet_config * config);

#endif
