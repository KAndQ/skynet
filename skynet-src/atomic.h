#ifndef SKYNET_ATOMIC_H
#define SKYNET_ATOMIC_H

<<<<<<< HEAD
/// ptr 指向的值和 oval 的值比较, 如果相等, 将 nval 的值赋给 ptr, 操作成功返回 true; 否则返回 false.
#define ATOM_CAS(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)

/// ptr 指向的值和 oval 的值比较, 如果相等, 将 nval 的值赋给 ptr, 操作成功返回 true; 否则返回 false.
#define ATOM_CAS_POINTER(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)

/// 将 ptr 指向的值加 1, 并且返回更新后的值
#define ATOM_INC(ptr) __sync_add_and_fetch(ptr, 1)

/// 将 ptr 指向的值加 1, 并且返回更新前的值
#define ATOM_FINC(ptr) __sync_fetch_and_add(ptr, 1)

/// 将 ptr 指向的值减 1, 并且返回更新后的值
#define ATOM_DEC(ptr) __sync_sub_and_fetch(ptr, 1)

/// 将 ptr 指向的值减 1, 并且返回更新前的值
#define ATOM_FDEC(ptr) __sync_fetch_and_sub(ptr, 1)

/// 将 ptr 指向的值加 n, 并且返回更新后的值
#define ATOM_ADD(ptr,n) __sync_add_and_fetch(ptr, n)

/// 将 ptr 指向的值减 n, 并且返回更新后的值
#define ATOM_SUB(ptr,n) __sync_sub_and_fetch(ptr, n)

/// 将 ptr 指向的值与 n 做 & 位运算, 并且返回更新后的值
=======
#define ATOM_CAS(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_CAS_POINTER(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_INC(ptr) __sync_add_and_fetch(ptr, 1)
#define ATOM_FINC(ptr) __sync_fetch_and_add(ptr, 1)
#define ATOM_DEC(ptr) __sync_sub_and_fetch(ptr, 1)
#define ATOM_FDEC(ptr) __sync_fetch_and_sub(ptr, 1)
#define ATOM_ADD(ptr,n) __sync_add_and_fetch(ptr, n)
#define ATOM_SUB(ptr,n) __sync_sub_and_fetch(ptr, n)
>>>>>>> cloudwu/master
#define ATOM_AND(ptr,n) __sync_and_and_fetch(ptr, n)

#endif
