#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(__APPLE__)
// Linux系统的日期时间头文件，sys/time.h 通常会包含 include <time.h>
#include <sys/time.h>
#endif

// 目前没有使用到
typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)	// 256
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)	// 64
#define TIME_NEAR_MASK (TIME_NEAR-1)		// 0xFF(11111111)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)		// 0x3F(00111111)

// 计时器时间, 记录是哪个 handle 定制的计时器, 与一个 timer_node 关联
struct timer_event {
	uint32_t handle;	// 关联的 context handle
	int session;		// 会话 session
};

// 计时器节点, 只记录期限时间, 通过和当前统计时间的差值, 调整 timer 的链表. 与一个 timer_event 关联
struct timer_node {
	struct timer_node *next;	// 链表的下一个 timer_node
	uint32_t expire;			// 计时器的期限时间, 以厘秒为单位
};

// timer_node 的链表
struct link_list {
	struct timer_node head;		// 其实链表头元素(timer_node)是从 head.next 开始
	struct timer_node *tail;	// 链表尾的指针
};

struct timer {
	// near[256], 保证计时的间隔是在 [0, 255] 这个区间内的短时间 timer_node, 加入到这个计时器链表数组中.
	struct link_list near[TIME_NEAR];

	// t[4][64], 对于大时间间隔的计时器, 这里对差值间隔分了 4 个数量级, 低 8 位之后的每 6 位为一个数量级.
	// 6 位用于计算在当前所在的层级中的索引(对应层级的小数量级分配区间), 每个大的层级里面划分了 64 个小的层级, 计算方式一般是 time & TIME_LEVEL_MASK, 具体查看 add_node 函数.
	// 注意: 除了 t[3][0] 中使用了 0 索引之外, 其他的 level 都不会使用 0 索引; 
	// 因为对于小的层级计算 time & TIME_LEVEL_MASK 如果等于 0 的话, 那么其实也就是还没进入到这个区间.

	// t[0]: 表示第一个数量级, [0x0100, 0x3FFF];
	// t[1]: 表示第二个数量级, [0x4000, 0xFFFFF];
	// t[2]: 表示第三个数量级, [0x100000, 0x3FFFFFF];
	// t[3]: 表示第四个数量级, [0x4000000, 0xFFFFFFFF];
	struct link_list t[4][TIME_LEVEL];
<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> parent of 5702862... Merge branch 'cloudwu/master'

	struct spinlock lock;		// 线程安全锁
	uint32_t time;				// 不断累加的计时计数, 可以理解为以厘秒为单位
	uint32_t current;			// 从 skynet 节点的启动系统时间开始, 每次 update 都会更新一次, 以厘秒为单位
	uint32_t starttime;			// 当前 skynet 的启动系统时间, 以秒为单位; 只有当 current 超过 0xffffffff 的时候, starttime 才会改变
	uint64_t current_point;		// 记录当前的运行时间, 以厘秒为单位
	uint64_t origin_point;		// 记录 skynet 节点的启动运行时间, 以厘秒为单位
=======
	struct spinlock lock;
	uint32_t time;
	uint32_t current;
	uint32_t starttime;
	uint64_t current_point;
	uint64_t origin_point;
>>>>>>> cloudwu/master
};

static struct timer * TI = NULL;

/// 清空 list 链表, 并返回链表头
static inline struct timer_node *
link_clear(struct link_list *list) {
	// 获得链表头
	struct timer_node * ret = list->head.next;

	// 重置为空链表
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

/// 将 node 添加到 list 的链表中
static inline void
link(struct link_list *list, struct timer_node *node) {
	// 这里有个有意思的地方, 当加入第一个元素的时候, list->head.next 是第一个链表的元素
	// 所以 head 本身其实不是链表首, 但是起到记录链表首的作用
	list->tail->next = node;
	list->tail = node;
	node->next = 0;
}

/**
 * 将 timer_node 添加到 timer 中.
 * 这里面的逻辑很有意思, timer 会根据 timer_node.expire 的值, 决定将该 timer_node 放入到对应的链表里.
 * @param T timer
 * @param node timer_node
 */
static void
add_node(struct timer * T, struct timer_node * node) {
	uint32_t time = node->expire;		// 注意, 这里的 expire 是根据 T->time 计算得到的, 在函数 timer_add 里面
	uint32_t current_time = T->time;
	
	// time 和 current_time 的差值在 [0, 255] 范围内, 
	// 将 node 加入到 near 链表数组的对应的链表里面, 对应链表的选择通过 (time & TIME_NEAR_MASK) 计算得到.
	if ((time | TIME_NEAR_MASK) == (current_time | TIME_NEAR_MASK)) {
		link(&T->near[time & TIME_NEAR_MASK], node);
	} else {
		// (0000 0000 0000 0000 0100 0000 0000 0000)
		uint32_t mask = TIME_NEAR << TIME_LEVEL_SHIFT;

		int i;
		for (i = 0; i < 3; i++) {
			// i = 0: mask - 1 => (0000 0000 0000 0000 0011 1111 1111 1111)
			// i = 1: mask - 1 => (0000 0000 0000 1111 1111 1111 1111 1111)
			// i = 2: mask - 1 => (0000 0011 1111 1111 1111 1111 1111 1111)
			// 判断当前所在的层级, 这里的运算是判断当前所在层级的最大数据是否相等,
			// 如果相等那么说明找到了所在的层级, 否则说明是更高的层级不相等, 继续判断(这里也可以理解为差值是否在同一个区间内的判断).
			if ((time | (mask - 1)) == (current_time | (mask - 1))) {
				break;
			}

			mask <<= TIME_LEVEL_SHIFT;
			// i = 0: mask => (0000 0000 0001 0000 0000 0000 0000 0000)
			// i = 1: mask => (0000 0100 0000 0000 0000 0000 0000 0000)
			// i = 2: mask => (0000 0000 0000 0000 0000 0000 0000 0000)
		}

		// i 是决定层级, 各个层次的描述在上面有描述.
		// ((time >> (TIME_NEAR_SHIFT + i * TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK), 这里计算得到的是对应链表的索引.
		
		// 这里会出现一种特殊的情况, 当 timer_node.expire < timer.time, 并且它们的差值大于 255, 并且 timer_node.expire < 0x4000000 的时候, 
		// timer_node 会被分配到 timer.t[3][0] 这个链表中!!!
		link(&T->t[i][((time >> (TIME_NEAR_SHIFT + i * TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)], node);
	}
}

/**
 * 给 T 添加一个计时器
 * @param T timer
 * @param arg 附加数据
 * @param sz 附加数据大小
 * @param time 计时时间
 */
static void
timer_add(struct timer * T, void *arg, size_t sz, int time) {

	// 分配 timer_node 的内存空间
	// 注意! 这里多分配了 sz 的内存容量, 用来存储 arg 的数据
	struct timer_node * node = (struct timer_node *)skynet_malloc(sizeof(*node) + sz);

	// 在 node 内存容量之后, 存储 arg 的数据,
	// 注意, 这里对 arg 的数据进行了复制.
	memcpy(node + 1, arg, sz);

<<<<<<< HEAD
	// 保证线程安全
=======
>>>>>>> cloudwu/master
	SPIN_LOCK(T);

	// 计算期满时间, 这个时候有可能 node->expire < T->time, 因为超过了 4294967295
	node->expire = time + T->time;

<<<<<<< HEAD
	// 将 node 添加到 timer 中, 当 expire 为 0 时的特殊处理已经在 add_node 中添加了说明
	add_node(T, node);

=======
>>>>>>> cloudwu/master
	SPIN_UNLOCK(T);
}

/**
 * 将 timer.t 中指定的链表清空, 并且链表内的数据会重新添加在 timer 中
 * @param T timer
 * @param level 层级
 * @param idx 索引
 */
static void
move_list(struct timer * T, int level, int idx) {

	// 获得 t[level][idx] 的链表, 并且清空 t[level][idx] 链表
	struct timer_node *current = link_clear(&T->t[level][idx]);

	// 将以 current 为链表头的链表元素重新再添加到 T 中
	while (current) {
		// 拿到下一个元素
		struct timer_node * temp = current->next;

		// 添加到 T 中
		add_node(T, current);

		// 继续下一次的计算
		current = temp;
	}
}

/**
 * 累加计时计数器, 并且对 timer 里面的链表根据当前的 time 进行调整.
 * 调整方式还是根据差值, 将 timer_node 再分配到对应的链表中. 
 */
static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;

	// 时间流逝
	uint32_t ct = ++T->time;

	// ct == 0, 表示 T->time 已经累加超过 4294967295, 重新从 0 开始计数.
	if (ct == 0) {

		// 会将之前因为越界问题分配到 t[3][0] 的 timer_node 重新再分配
		move_list(T, 3, 0);

	// 在没有超过 4294967295 时做的运算, 主要会重新分配 timer_node 到其适合的链表
	} else {
		// 位运算, 移除掉 NEAR 的数据位
		uint32_t time = ct >> TIME_NEAR_SHIFT;

		int i = 0;

		// 针对 timer_node.expire 和 timer.time 的差值, 对 timer 内的链表进行调整
		while ((ct & (mask - 1)) == 0) {	// 首先, 必须每次经过 255 才会进入第一次的运算; 接下来的进入条件是每次有大层级进位的时候, 例如: ct 从 0x3FFF -> 0x4000
			
			// 计算所在的小层级
			int idx = time & TIME_LEVEL_MASK;

			if (idx != 0) {

				// 根据 timer_node.expire 和 timer.time 的差值, 调整链表.
				// 其实是将 timer_node 从差值大的链表根据当前的 timer.time, 转移到差值小的链表中.
				move_list(T, i, idx);
				break;
			}

			// 为下一个大层级区间提供 mask
			mask <<= TIME_LEVEL_SHIFT;

			// 位运算, 将当前层级的 6 位移除掉
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}

/// 将 timer_node 链的所有 timer_node 关联的 event 作为一个 skynet_message, 压入 event.handle 关联的 context 队列.
static inline void
dispatch_list(struct timer_node *current) {
	do {
		// 查看 timer_add 和 skynet_timeout 函数, 可以知道为什么 current + 1 之后可以得到 timer_event 的指针
		struct timer_event * event = (struct timer_event *)(current + 1);

		// 将消息压入到 event->handle 所在的 context 信息队列里面
		struct skynet_message message;
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		skynet_context_push(event->handle, &message);
		
		// 拿到链表的下一个元素
		struct timer_node * temp = current;
		current = current->next;

		// 释放 timer_node 内存资源
		skynet_free(temp);	
	} while (current);
}

static inline void
timer_execute(struct timer *T) {
	
	// 拿到对应的索引
	int idx = T->time & TIME_NEAR_MASK;
	
	// 将到时间的 timer_node 取出来, 然后给各自的 context 发送消息
	while (T->near[idx].head.next) {
		// 拿到链表头元素
		struct timer_node *current = link_clear(&T->near[idx]);
<<<<<<< HEAD

		SPIN_UNLOCK(T);		// !!! UNLOCK, 可以让其他的线程继续操作 T
		
<<<<<<< HEAD
=======
		SPIN_UNLOCK(T);
>>>>>>> cloudwu/master
=======
>>>>>>> parent of 5702862... Merge branch 'cloudwu/master'
		// dispatch_list don't need lock T
		// dispatch_list 函数不需要锁住 T
		dispatch_list(current);
<<<<<<< HEAD

		SPIN_LOCK(T);		// !!! LOCK, 锁住, 后面要继续使用 T
=======
		SPIN_LOCK(T);
>>>>>>> cloudwu/master
	}
}

/// timer 的主要逻辑更新
static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	// 尝试触发超时为 0 的计时器(罕见的情况)
	timer_execute(T);

	// shift time first, and then dispatch timer message
	// 首先移动时间节点, 之后在派发计时器信息
	timer_shift(T);

	// 这里就是正常的时间派发了
	timer_execute(T);

	SPIN_UNLOCK(T);
}

/// 创建 struct timer
static struct timer *
timer_create_timer() {
	// 分配 timer 的内存空间, 并且初始化
	struct timer * r = (struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r, 0, sizeof(*r));

	int i, j;

	// 初始化 timer.near 内的链表
	for (i = 0; i < TIME_NEAR; i++) {
		link_clear(&r->near[i]);
	}

	// 初始化 timer.t 内的链表
	for (i = 0; i < 4; i++) {
		for (j = 0; j < TIME_LEVEL; j++) {
			link_clear(&r->t[i][j]);
		}
	}

	SPIN_INIT(r)

	r->current = 0;

	return r;
}

int
skynet_timeout(uint32_t handle, int time, int session) {

	// 因为 time 参数是 0, 所以没必要再添加到计时器中,
	// 直接将信息压入到对应的 skynet_context 中.
	if (time == 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		// 添加 timer_node
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second
// 厘秒: 百分之一秒
/**
 * 得到当前的系统时间
 * @param sec 获取当前系统时间, 以秒为单位
 * @param cs 获得当前系统时间, 以厘秒为单位
 */
static void
systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__)
	
	// struct timespec
	// {
	//     time_t tv_sec;	// 秒
	//     long tv_nsec;	// 1秒内的纳秒数
	// };
	struct timespec ti;

	// 函数 "clock_gettime" 是基于 Linux C 语言的时间函数, 他可以用于计算精度和纳秒.
	// int clock_gettime(clockid_t clk_id, struct timespec * tp);
	// clk_id: 检索和设置的clk_id指定的时钟时间。
	// CLOCK_REALTIME: 系统实时时间,随系统实时时间改变而改变, 即从UTC1970-1-1 0:0:0开始计时, 中间时刻如果系统时间被用户改成其他, 则对应的时间相应改变;
	// CLOCK_MONOTONIC: 从系统启动这一刻起开始计时, 不受系统时间被用户改变的影响;
	// CLOCK_PROCESS_CPUTIME_ID: 本进程到当前代码系统CPU花费的时间;
	// CLOCK_THREAD_CPUTIME_ID: 本线程到当前代码系统CPU花费的时间;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
#else

	// struct timeval {
	//     long int tv_sec;		// 秒数
	//     long int tv_usec;	// 1 秒内的微秒数
	// }
	struct timeval tv;

	// 使用C语言编写程序需要获得当前精确时间（1970年1月1日到现在的时间），或者为执行计时，可以使用gettimeofday()函数。
	// int gettimeofday(struct timeval * tv, struct timezone * tz);
	// 其参数tv是保存获取时间结果的结构体，参数tz用于保存时区结果：
	// struct timezone {
	//     int tz_minuteswest; /* 格林威治时间往西方的时差 */
	//     int tz_dsttime; /* DST 时间的修正方式 */
	// }
	// timezone 参数若不使用则传入NULL即可。
	gettimeofday(&tv, NULL);
	*sec = tv.tv_sec;
	*cs = tv.tv_usec / 10000;
#endif
}

/// 获得当前的厘秒数
static uint64_t
gettime() {
	uint64_t t;

#if !defined(__APPLE__)

#ifdef CLOCK_MONOTONIC_RAW

// (since Linux 2.6.28; Linux-specific)
// Similar to CLOCK_MONOTONIC, but provides access to a raw hard‐
// ware-based time that is not subject to NTP adjustments.
// 模拟 CLOCK_MONOTONIC 模式, 但是提供访问原生硬件层的时间, 不服从于 NTP(网络时间协议) 的调整.
#define CLOCK_TIMER CLOCK_MONOTONIC_RAW
#else

// Clock that cannot be set and represents monotonic time since some unspecified starting point.
// 不能够被设置的时钟, 表示的是从某个未指定的起始点单调递增的时间.
#define CLOCK_TIMER CLOCK_MONOTONIC
#endif

	struct timespec ti;
	clock_gettime(CLOCK_TIMER, &ti);
	t = (uint64_t)ti.tv_sec * 100;

	// 因为 tv_nsec 表示的是 1 秒内的纳秒数
	t += ti.tv_nsec / 10000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint64_t)tv.tv_sec * 100;

	// 因为 tv_usec 表示的是 1 秒内的微妙数
	t += tv.tv_usec / 10000;
#endif
	return t;
}

void
skynet_updatetime(void) {

	// 得到运行时间
	uint64_t cp = gettime();

	if(cp < TI->current_point) {
		
		// 这种情况一般不会发生, 如果发生了, 我想应该是程序在运行的时候, 修改了系统的时间, 从而使 cp 的时间变小了,
		// 然后这个时候强行让 current_point 的时间遵从 cp 的时间, 纠正.
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;

	} else if (cp != TI->current_point) {

		// 得到当前时间和上次记录时间的时间间隔
		uint32_t diff = (uint32_t)(cp - TI->current_point);

		// 记录本次的运行时间
		TI->current_point = cp;

		// 系统时间统计的更新
		uint32_t oc = TI->current;
		TI->current += diff;

		// 说明 current 已经累计并超过 0xffffffff
		if (TI->current < oc) {
			// when cs > 0xffffffff(about 497 days), time rewind
			// 当 cs > 0xffffffff(大约 497 天), 时间倒回
			TI->starttime += 0xffffffff / 100;
		}

		// 各个计时器的更新, 经过多少厘秒, 就循环更新多少次
		int i;
		for (i = 0; i < diff; i++) {
			timer_update(TI);
		}
	}
}

uint32_t
skynet_gettime_fixsec(void) {
	return TI->starttime;
}

uint32_t 
skynet_gettime(void) {
	return TI->current;
}

void 
skynet_timer_init(void) {
	TI = timer_create_timer();

	// 拿到当前 skynet 节点启动的系统时间
	systime(&TI->starttime, &TI->current);

	// 拿到当前 skynet 节点启动的运行时间
	uint64_t point = gettime();
	TI->current_point = point;
	TI->origin_point = point;
}

