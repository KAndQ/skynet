#ifndef skynet_databuffer_h
#define skynet_databuffer_h

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MESSAGEPOOL 1023	// message 缓存的大小

// 存储数据的基本单元
struct message {
	char * buffer;	// 数据指针
	int size;		// 数据大小
	struct message * next;	// 下一个 message 节点
};

// 存储 message 的队列, 读取的数据存在里面
struct databuffer {
	int header;				// 需要读取数据的长度
	int offset;				// 读取当前 message 的指针偏移量
	int size;				// 存储总数据的大小
	struct message * head;	// 队列头
	struct message * tail;	// 队列尾(最新加入的元素)
};

// 存放一组连续的 message 对象
struct messagepool_list {
	struct messagepool_list *next;		// 指向下一个 messagepool_list
	struct message pool[MESSAGEPOOL];	// 预先分配的一组 message
};

// 管理 message, 分配的 message 都由 messagepool 来管理
struct messagepool {
	struct messagepool_list * pool;		// messagepool_list 栈结构
	struct message * freelist;			// 存放当前可用的 message, 栈结构
};

// use memset init struct 
// 使用 memset 初始化结构体

/// 释放 messagepool 对象的内部资源
static void 
messagepool_free(struct messagepool *pool) {
	struct messagepool_list *p = pool->pool;
	while(p) {
		struct messagepool_list *tmp = p;
		p=p->next;
		skynet_free(tmp);
	}
	pool->pool = NULL;
	pool->freelist = NULL;
}

/// 从 db 中取出 message, 释放 message.buffer 数据, 将 message 加入到 messagepool 中
static inline void
_return_message(struct databuffer *db, struct messagepool *mp) {
	struct message *m = db->head;
	if (m->next == NULL) {	// databuffer 中的最后一个元素
		assert(db->tail == m);
		db->head = db->tail = NULL;
	} else {
		db->head = m->next;
	}
	skynet_free(m->buffer);
	m->buffer = NULL;
	m->size = 0;

	// 将 message 加入到 messagepool.freelist 中
	m->next = mp->freelist;
	mp->freelist = m;
}

/// 从 databuffer 中读取 sz 大小的数据到 buffer 中.
static void
databuffer_read(struct databuffer *db, struct messagepool *mp, void * buffer, int sz) {
	assert(db->size >= sz);
	db->size -= sz;
	for (;;) {
		struct message *current = db->head;
		int bsz = current->size - db->offset;	// 计算 message 中剩余可读数据
		
		if (bsz > sz) {		// 剩余数据大于期望读取数据
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset += sz;
			return;
		}

		if (bsz == sz) {	// 恰好足够的数据可读
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset = 0;
			_return_message(db, mp);
			return;
		} else {			// 剩余数据不足
			memcpy(buffer, current->buffer + db->offset, bsz);
			_return_message(db, mp);
			db->offset = 0;
			buffer+=bsz;
			sz-=bsz;
		}
	}
}

/// 将 data 数据存入到 dababuffer 中
static void
databuffer_push(struct databuffer *db, struct messagepool *mp, void *data, int sz) {
	struct message * m;
	if (mp->freelist) {	// 从 messagepool 中取出可用的 message
		m = mp->freelist;
		mp->freelist = m->next;
	} else {
		// 创建 messagepool_list 对象
		struct messagepool_list * mpl = skynet_malloc(sizeof(*mpl));
		struct message * temp = mpl->pool;

		// 初始化 messagepool_list 中的 message
		int i;
		for (i=1;i<MESSAGEPOOL;i++) {
			temp[i].buffer = NULL;
			temp[i].size = 0;
			temp[i].next = &temp[i+1];
		}
		temp[MESSAGEPOOL-1].next = NULL;

		// 将 messagepool_list 压入 message_pool 中
		mpl->next = mp->pool;
		mp->pool = mpl;

		m = &temp[0];	// 取出当前需要使用的 message
		mp->freelist = &temp[1];	// 记录可用的 message
	}

	// 将 buffer 压入到 databuffer 中
	m->buffer = data;
	m->size = sz;
	m->next = NULL;
	db->size += sz;
	if (db->head == NULL) {
		assert(db->tail == NULL);
		db->head = db->tail = m;
	} else {
		db->tail->next = m;
		db->tail = m;
	}
}

/// 从 databuffer 中读取数据头, header_size (2 or 4)个字节大小.
/// 返回值, 如果接下来可读的数据足够, 返回需要读取的数据大小, 否则返回 -1
static int
databuffer_readheader(struct databuffer *db, struct messagepool *mp, int header_size) {
	if (db->header == 0) {
		// parser header (2 or 4)
		// 解析 header (2 或者 4 个字节)
		if (db->size < header_size) {
			return -1;
		}

		uint8_t plen[4];
		databuffer_read(db, mp, (char *)plen, header_size);
		
		// big-endian, 高位在低字节
		if (header_size == 2) {
			db->header = plen[0] << 8 | plen[1];
		} else {
			db->header = plen[0] << 24 | plen[1] << 16 | plen[2] << 8 | plen[3];
		}
	}

	// 判断可读的数据是否足够
	if (db->size < db->header)
		return -1;

	return db->header;
}

/// 设置 databuffer.header 为 0
static inline void
databuffer_reset(struct databuffer *db) {
	db->header = 0;
}

/// 释放 databuffer 的资源, 将 databuffer 所有数据重置.
static void
databuffer_clear(struct databuffer *db, struct messagepool *mp) {
	while (db->head) {
		_return_message(db,mp);
	}
	memset(db, 0, sizeof(*db));
}

#endif
