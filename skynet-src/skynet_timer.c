#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"

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

// 保证线程安全的一些相关操作
#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}
#define UNLOCK(q) __sync_lock_release(&(q)->lock);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)	// 256
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)	// 64
#define TIME_NEAR_MASK (TIME_NEAR-1)		// 0xFF(11111111)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)		// 0x3F(00111111)

struct timer_event {
	uint32_t handle;
	int session;
};

struct timer_node {
	struct timer_node *next;	// 链表的下一个 timer_node
	uint32_t expire;			// 计时器的时限
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
	// 6 位表示的在当前所在的层级中的索引, 索引具体的计算方式, 参考函数 add_node.
	// t[0]: 表示第一个数量级, (0x4000, 0x0100], 差值在 16128 这个范围内;
	// t[1]: 表示第二个数量级, (0x100000, 0x4000], 差值在 1032192 这个范围内;
	// t[2]: 表示第三个数量级, (0x4000000, 0x100000], 差值在 66060288 这个范围内;
	// t[3]: 表示第四个数量级, (0x100000000, 0x4000000], 差值在 4227858432 这个范围内;
	struct link_list t[4][TIME_LEVEL];
	int lock;							// 线程安全锁
	uint32_t time;
	uint32_t current;
	uint32_t starttime;
	uint64_t current_point;
	uint64_t origin_point;
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
	uint32_t time = node->expire;		// 注意, 这里的 time 是根据 T->time 计算得到的, 在函数 timer_add 里面
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
			// 如果相等那么说明找到了所在的层级, 否则说明是更高的层级不相等, 继续判断.
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

	// 在 node 内存容量之后, 存储 arg 的数据
	memcpy(node + 1, arg, sz);

	// 保证线程安全
	LOCK(T);

	// 计算期满时间, 这个时候有可能 node->expire < T->time, 因为超过了 4294967295
	node->expire = time + T->time;

	// 将 node 添加到 timer 中, 当 expire 为 0 时的特殊处理已经在 add_node 中添加了说明
	add_node(T, node);

	UNLOCK(T);
}

/**
 * 将 timer.t 中指定的链表清空, 并且链表内的数据重新在添加会 timer 中
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

static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;

	// 时间流逝
	uint32_t ct = ++T->time;

	// ct == 0, 表示 T->time 已经累加超过 4294967295, 重新从 0 开始计数.
	if (ct == 0) {

		// 
		move_list(T, 3, 0);

	// 
	} else {
		uint32_t time = ct >> TIME_NEAR_SHIFT;
		int i = 0;

		while ((ct & (mask - 1)) == 0) {
			int idx = time & TIME_LEVEL_MASK;
			if (idx != 0) {
				move_list(T, i, idx);
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}

static inline void
dispatch_list(struct timer_node *current) {
	do {
		struct timer_event * event = (struct timer_event *)(current+1);
		struct skynet_message message;
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = PTYPE_RESPONSE << HANDLE_REMOTE_SHIFT;

		skynet_context_push(event->handle, &message);
		
		struct timer_node * temp = current;
		current=current->next;
		skynet_free(temp);	
	} while (current);
}

static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;
	
	while (T->near[idx].head.next) {
		struct timer_node *current = link_clear(&T->near[idx]);
		UNLOCK(T);
		// dispatch_list don't need lock T
		dispatch_list(current);
		LOCK(T);
	}
}

static void 
timer_update(struct timer *T) {
	LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	timer_execute(T);

	// shift time first, and then dispatch timer message
	timer_shift(T);

	timer_execute(T);

	UNLOCK(T);
}

static struct timer *
timer_create_timer() {
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	r->lock = 0;
	r->current = 0;

	return r;
}

int
skynet_timeout(uint32_t handle, int time, int session) {
	if (time == 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = PTYPE_RESPONSE << HANDLE_REMOTE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second
static void
systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	*sec = tv.tv_sec;
	*cs = tv.tv_usec / 10000;
#endif
}

static uint64_t
gettime() {
	uint64_t t;
#if !defined(__APPLE__)

#ifdef CLOCK_MONOTONIC_RAW
#define CLOCK_TIMER CLOCK_MONOTONIC_RAW
#else
#define CLOCK_TIMER CLOCK_MONOTONIC
#endif

	struct timespec ti;
	clock_gettime(CLOCK_TIMER, &ti);
	t = (uint64_t)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint64_t)tv.tv_sec * 100;
	t += tv.tv_usec / 10000;
#endif
	return t;
}

void
skynet_updatetime(void) {
	uint64_t cp = gettime();
	if(cp < TI->current_point) {
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} else if (cp != TI->current_point) {
		uint32_t diff = (uint32_t)(cp - TI->current_point);
		TI->current_point = cp;

		uint32_t oc = TI->current;
		TI->current += diff;
		if (TI->current < oc) {
			// when cs > 0xffffffff(about 497 days), time rewind
			TI->starttime += 0xffffffff / 100;
		}
		int i;
		for (i=0;i<diff;i++) {
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
	systime(&TI->starttime, &TI->current);
	uint64_t point = gettime();
	TI->current_point = point;
	TI->origin_point = point;
}

