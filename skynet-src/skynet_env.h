/**
 * skynet_env 主要功能是让不同的服务(因为不同的服务是使用不同的 lua 沙箱)能够访问一些相同的数据.
 * 在当前节点内, 各个服务间可共享的数据, 一般用于存储节点的配置数据. 字典数据结构.
 */

#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H

/**
 * 得到一个 key 对应的值, 该值是个字符串.
 * @param key 键
 * @return key 对应的值
 */
const char * skynet_getenv(const char *key);

/**
 * 设置 key 对应的值
 * @param key 键
 * @param value 值
 */
void skynet_setenv(const char *key, const char *value);

/// 当前节点的全局变量初始化
void skynet_env_init();

#endif
