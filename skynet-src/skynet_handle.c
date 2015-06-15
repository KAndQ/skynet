#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000

// 为了 handle 和 name 相互查询的数据结构
struct handle_name {
	char * name;
	uint32_t handle;
};

// handle 的管理器, 管理进程(节点) context/handle/name 之间的关联
struct handle_storage {
	struct rwlock lock;		// 线程锁

	uint32_t harbor;		// 当前进程(节点)的 harbor
	uint32_t handle_index;	// 当前可用的 slot 索引, 从 1 初始化, handle_index 永远都是增加的, 所以可以保证 handle_index 永远不为 0, 但是通过位操作, 可以保证得到的索引为 0.
	int slot_size;			// 当前 slot 的大小
	struct skynet_context ** slot;	// 存储 skynet_context 指针的数组, 例: struct skynet_context * slot[DEFAULT_SLOT_SIZE]
	
	int name_cap;			// 能够存储 handle_name 数据的上限大小
	int name_count;			// 表示已经存储 handle_name 的数量
	struct handle_name *name;	// 存储 handle_name 数据结构的数组, 例: struct handle_name name[2];
};
static struct handle_storage *H = NULL;

uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	// 线程上锁
	rwlock_wlock(&s->lock);
	
	for (;;) {	// 注意, 这里是一个无限循环, 必须拿到一个可用的 handle
		int i;

		// 从头开始循环遍历
		for (i=0;i<s->slot_size;i++) {
			uint32_t handle = (i+s->handle_index) & HANDLE_MASK;	// 获得从右到左 3 个字节的数据, 最左的 4 个字节用来表示 harbor
			int hash = handle & (s->slot_size-1);					// 获得实际的索引, hash 的值不超出 slot_size 的范围

			// 当有可用的 slot 放入 ctx
			if (s->slot[hash] == NULL) {

				// 记录注册的 ctx
				s->slot[hash] = ctx;

				// handle_index 自增
				s->handle_index = handle + 1;

				// 已经存储了 ctx 了, 可以解锁让其他线程使用了
				rwlock_wunlock(&s->lock);

				// 新的 handle 需要使用最左边的第 4 个字节记录当前 handle 所属的 harbor
				handle |= s->harbor;
				return handle;
			}
		}

		// 如果没有可用的空间, 扩展容量
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);

		// 申请新的内存空间
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));

		// 重置内存数据
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));

		// 将原来的内容进行复制
		for (i=0;i<s->slot_size;i++) {
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);	// 获得在当前分配的空间中可用的索引, 注意这里是使用  (slot_size * 2 - 1) 在进行为操作.
			assert(new_slot[hash] == NULL);

			// 这里为什么不是直接 new_slot[i] = s->slot[i] 呢, 而要绕个弯拿到 hash, 然后 new_slot[hash] = s->slot[i]
			// 解答, 很有意思的小细节:
			// 首先说 handle_index 从 1 开始计数, 而且都是不断的增加, 返回的时候使用的是 return handle |= s->harbor; 这就决定了返回值不会为 0
			// 在上面的查询 slot, 并且分配 handle 的过程中, 索引是 hash, 返回值是 handle, handle 可以大于 slot_size.
			// 这里获得的 hash 可以是大于 slot_size 的, 那么现在可以举例: hash = 4, i = 0
			// new_slot[4] = s->slot[0]
			new_slot[hash] = s->slot[i];
		}

		// 释放之前的数据
		skynet_free(s->slot);

		// 记录新的数据
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}

int
skynet_handle_retire(uint32_t handle) {
	int ret = 0;
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		s->slot[hash] = NULL;
		ret = 1;
		int i;
		int j=0, n=s->name_count;
		for (i=0; i<n; ++i) {
			if (s->name[i].handle == handle) {
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				s->name[j] = s->name[i];
			}
			++j;
		}
		s->name_count = j;
	} else {
		ctx = NULL;
	}

	rwlock_wunlock(&s->lock);

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.
		skynet_context_release(ctx);
	}

	return ret;
}

void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx)
				handle = skynet_context_handle(ctx);
			rwlock_runlock(&s->lock);
			if (handle != 0) {
				if (skynet_handle_retire(handle)) {
					++n;
				}
			}
		}
		if (n==0)
			return;
	}
}

struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	} else {
		int i;
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	char * result = skynet_strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

void 
skynet_handle_init(int harbor) {
	assert(H==NULL);
	struct handle_storage * s = skynet_malloc(sizeof(*H));
	s->slot_size = DEFAULT_SLOT_SIZE;
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	rwlock_init(&s->lock);
	// reserve 0 for system
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1;
	s->name_cap = 2;
	s->name_count = 0;
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

