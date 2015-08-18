/**
 * 对 lua 数据结构初始化的数据方案.
 * 这个序列化库支持 string, boolean, number, lightuserdata, table 这些类型，
 * 但对 lua table 的 metatable 支持非常有限，所以尽量不要用其打包带有元方法的 lua 对象。
 */

#ifndef LUA_SERIALIZE_H
#define LUA_SERIALIZE_H

#include <lua.h>

/// 可以将一组 lua 对象序列化为一个由 malloc 分配出来的 C 指针加一个数字长度。你需要考虑 C 指针引用的数据块何时释放的问题。
int _luaseri_pack(lua_State *L);

/// 它可以把一个 C 指针加长度的消息解码成一组 Lua 对象。
int _luaseri_unpack(lua_State *L);

#endif
