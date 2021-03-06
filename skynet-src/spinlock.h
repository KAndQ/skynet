/**
 * 线程锁的公共接口声明/定义.
 */

#ifndef SKYNET_SPINLOCK_H
#define SKYNET_SPINLOCK_H

/* 宏定义公共接口 */

#define SPIN_INIT(q) spinlock_init(&(q)->lock);
#define SPIN_LOCK(q) spinlock_lock(&(q)->lock);
#define SPIN_UNLOCK(q) spinlock_unlock(&(q)->lock);
#define SPIN_DESTROY(q) spinlock_destroy(&(q)->lock);

#ifndef USE_PTHREAD_LOCK

/// 自定义自旋锁数据结构
struct spinlock {
	int lock;
};

/// 初始化
static inline void
spinlock_init(struct spinlock *lock) {
	lock->lock = 0;
}

/// 上锁
static inline void
spinlock_lock(struct spinlock *lock) {
	while (__sync_lock_test_and_set(&lock->lock,1)) {}
}

/// 尝试获得锁, 如果成功获得锁返回 1, 否则返回 0
static inline int
spinlock_trylock(struct spinlock *lock) {
	return __sync_lock_test_and_set(&lock->lock,1) == 0;
}

/// 解锁
static inline void
spinlock_unlock(struct spinlock *lock) {
	__sync_lock_release(&lock->lock);
}

/// 销毁锁
static inline void
spinlock_destroy(struct spinlock *lock) {
	(void) lock;
}

#else

#include <pthread.h>

// we use mutex instead of spinlock for some reason
// you can also replace to pthread_spinlock
// 我们因为某些原因使用互斥量代替自旋锁
// 你也可以使用 pthread_spinlock_t 做替换

/*
Pthreads提供了多种锁机制,常见的有：
	1) Mutex（互斥量）：pthread_mutex_***
	2) Spin lock（自旋锁）：pthread_spin_***
	3) Condition Variable（条件变量）：pthread_con_***
	4) Read/Write lock（读写锁）：pthread_rwlock_***
UNIX 环境高级编程, 11 章-线程-线程同步, 有介绍各个锁

对于互斥量和自旋锁的性能分析, http://www.cnblogs.com/diyunpeng/archive/2011/06/07/2074059.html
 */

// 以前接口的声明定义, 同上

struct spinlock {
	pthread_mutex_t lock;
};

static inline void
spinlock_init(struct spinlock *lock) {
	pthread_mutex_init(&lock->lock, NULL);
}

static inline void
spinlock_lock(struct spinlock *lock) {
	pthread_mutex_lock(&lock->lock);
}

static inline int
spinlock_trylock(struct spinlock *lock) {
	return pthread_mutex_trylock(&lock->lock) == 0;
}

static inline void
spinlock_unlock(struct spinlock *lock) {
	pthread_mutex_unlock(&lock->lock);
}

static inline void
spinlock_destroy(struct spinlock *lock) {
	pthread_mutex_destroy(&lock->lock);
}

#endif

#endif
