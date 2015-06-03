/**
 * skynet_env 主要功能是让不同的服务(因为不同的服务是使用不同的 lua 沙箱)能够访问一些相同的数据.
 * 存储的值只能是字符串类型. 也可以理解为当前节点的一个小缓存数据库.
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

/**
 * 初始化
 */
void skynet_env_init();

#endif
