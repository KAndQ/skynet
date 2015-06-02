#ifndef SKYNET_H
#define SKYNET_H

#include "skynet_malloc.h"

// 定义了一些标准宏以及类型.
#include <stddef.h>

// stdint.h是c99中引进的一个标准C库的头文件.
// C99中，<stdint.h>中定义了几种扩展的整数类型和宏。规则如下(其中N可以为8，16，32，64)
// intN_t, int_leastN_t, int_fastN_t表示长度为N位的整型数；
// uintN_t, uint_leastN_t, uint_fastN_t表示长度为N位的无符号整型数；
// stdint.h中的常量，定义以上各类型数的最大最小值(其中N可以为8，16，32，64)
// INTN_MIN, UINTN_MIN, INTN_MAX, UINTN_MAX;
// INT_LEASEN_MIN, INT_LEASEN_MAX;
// INT_FASTN_MIN, INT_FASTN_MAX;
#include <stdint.h>

// 以下宏是消息类别的定义, 每个服务可以接收 256 种不同类别的消息。每种类别可以有不同的消息编码格式。
// 有十几种类别是框架保留的，通常也不建议用户定义新的消息类别。
// 因为用户完全可以利用已有的类别，而用具体的消息内容来区分每条具体的含义。
// 框架把这些 type 映射为字符串便于记忆。最常用的消息类别名为 "lua" 广泛用于用 lua 编写的 skynet 服务间的通讯。

#define PTYPE_TEXT 0
#define PTYPE_RESPONSE 1
#define PTYPE_MULTICAST 2
#define PTYPE_CLIENT 3
#define PTYPE_SYSTEM 4
#define PTYPE_HARBOR 5
#define PTYPE_SOCKET 6
// read lualib/skynet.lua examples/simplemonitor.lua
#define PTYPE_ERROR 7	
// read lualib/skynet.lua lualib/mqueue.lua lualib/snax.lua
#define PTYPE_RESERVED_QUEUE 8
#define PTYPE_RESERVED_DEBUG 9
#define PTYPE_RESERVED_LUA 10
#define PTYPE_RESERVED_SNAX 11

#define PTYPE_TAG_DONTCOPY 0x10000
#define PTYPE_TAG_ALLOCSESSION 0x20000

// 关于 skynet_context 数据结构, 会在 skynet_server.c 中详细说明.
struct skynet_context;

/**
 * skynet 框架打印信息
 * @param context skynet_context
 * @param msg 打印信息
 */
void skynet_error(struct skynet_context * context, const char *msg, ...);

/**
 * 执行 skynet 中指定的命令函数
 * @param context skynet_context
 * @param cmd 执行的命令字符串, 可以在 skynet_server.c 的 cmd_funcs 中看到支持的命令
 * @param parm 使用 cmd 得到执行函数传入的参数
 * @return 命令对应执行函数的返回值
 */
const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);

/**
 * 使用 name 查询到对应的 handle(整型), 这里类似在 lua 中调用 skynet.localname
 * @param context skynet_context
 * @param name 用来查询的名字, name 的格式会在 skynet.handle.c 中详细说明, 也可以看 https://github.com/cloudwu/skynet/wiki/LuaAPI
 * @return 返回的 name 对应的 handle, 如果没有找到则返回 0.
 */
uint32_t skynet_queryname(struct skynet_context * context, const char * name);

/**
 * skynet 服务间发送消息
 * @param context skynet_context 
 * @param source 发送源, 为 0 表示发送源是 context
 * @param destination 目标服务, 为 0 表示不发送给其他服务
 * @param type 就是本文件上面定义的消息类型
 * @param session 大部分消息工作在请求回应模式下。
 * 即，一个服务向另一个服务发起一个请求，而后收到请求的服务在处理完请求消息后，回复一条消息。
 * session 是由发起请求的服务生成的，对它自己唯一的消息标识。回应方在回应时，将 session 带回。
 * 这样发送方才能识别出哪条消息是针对哪条的回应。session 是一个非负整数，当一条消息不需要回应时，按惯例，使用 0 这个特殊的 session 号。
 * session 由 skynet 框架生成管理，通常不需要使用者关心。
 * @param msg 发送的内容
 * @param sz 发送内容的数据长度
 * @return 失败返回 -1, 否则返回与 session 相同的值
 */
int skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * msg, size_t sz);

/**
 * 同上面的函数, 只是 destination 换成了使用服务的名字.
 */
int skynet_sendname(struct skynet_context * context, uint32_t source, const char * destination , int type, int session, void * msg, size_t sz);

/**
 * 判断 handle 对应的服务是否是远程节点
 * @param context skynet_context
 * @param handle 待判断的服务 handle
 * @param harbor 得到 handle 对应的 harbor
 * @return 1 表示是远程节点, 否则反之.
 */
int skynet_isremote(struct skynet_context *, uint32_t handle, int * harbor);

// 会在 skynet_server.c 的 dispatch_message 中详细说明这个函数类型的功能.
typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);

/**
 * 将 ud 和 cb 传递给 context
 * @param context skynet_context
 * @param ud 对应 context->cb_ud
 * @param cb 对应 context->cb
 */
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

// 当前服务的 handle 值, 在 skynet_server.c 中会详细介绍这个函数.
uint32_t skynet_current_handle(void);

#endif
