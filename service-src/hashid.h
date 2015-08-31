#ifndef skynet_hashid_h
#define skynet_hashid_h

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/// hash 节点
struct hashid_node {
	int id;		// id
	struct hashid_node *next;	// 在 hash.hash 中使用, 关联的下一个节点
};

/*
hash 表
hash.id 是保证是要还有可用的 hash_node
hash.hash 是管理当插入的 id 或者运算后的模值相同时的 hash_node
*/
struct hashid {
	int hashmod;	// 模, 也是 hash.hash 的最大容量
	int cap;		// hash.id 最大容量
	int count;		// hash.id 当前存储数量

	// hash_node 的数组
	// 当向 hash.id 中插入 id 的时候, 使用的索引是可用的索引
	struct hashid_node *id;

	// 管理 hash_node 的指针(栈)
	// 当向 hash.hash 中插入 id 的时候, 使用的索引是与 hash.mod 取模运算后的值
	struct hashid_node **hash;
};

/// 初始化 hash, max 表示最大的存储容量
static void
hashid_init(struct hashid *hi, int max) {
	int i;
	int hashcap;

	hashcap = 16;
	while (hashcap < max) {
		hashcap *= 2;
	}
	hi->hashmod = hashcap - 1;	// 这个是希望表示 hashcap 有几率可能与 max 相同?

	hi->cap = max;
	hi->count = 0;

	// 初始化 hash.id
	hi->id = skynet_malloc(max * sizeof(struct hashid_node));
	for (i=0;i<max;i++) {
		hi->id[i].id = -1;
		hi->id[i].next = NULL;
	}

	// 初始化 hash.hash
	hi->hash = skynet_malloc(hashcap * sizeof(struct hashid_node *));
	memset(hi->hash, 0, hashcap * sizeof(struct hashid_node *));
}

/// 清除 hash 管理的内容, 但是不 free hi
static void
hashid_clear(struct hashid *hi) {
	skynet_free(hi->id);
	skynet_free(hi->hash);
	hi->id = NULL;
	hi->hash = NULL;
	hi->hashmod = 1;
	hi->cap = 0;
	hi->count = 0;
}

/// 查询 hash_node, 返回 hash_node 与 hash.id 的指针偏移量; 如果没有查询到则返回 -1.
static int
hashid_lookup(struct hashid *hi, int id) {
	int h = id & hi->hashmod;
	struct hashid_node * c = hi->hash[h];
	while(c) {
		if (c->id == id)
			return c - hi->id;
		c = c->next;
	}
	return -1;
}

/// 移除 id 对应的 hash_node. 只要找到 hash_node.id == id 的就移除返回(仅移除 1 个 hash_node).
/// 返回值是 hash_node 在 hash.id 中的指针偏移量.
static int
hashid_remove(struct hashid *hi, int id) {
	int h = id & hi->hashmod;
	struct hashid_node * c = hi->hash[h];
	if (c == NULL)
		return -1;

	// 判断第一个元素是否相等
	if (c->id == id) {
		hi->hash[h] = c->next;
		goto _clear;
	}

	// 接下来的链表是否有数据相等
	while(c->next) {
		if (c->next->id == id) {
			struct hashid_node * temp = c->next;
			c->next = temp->next;
			c = temp;
			goto _clear;
		}
		c = c->next;
	}
	return -1;

_clear:
	c->id = -1;
	c->next = NULL;
	--hi->count;
	return c - hi->id;
}

/// 将 id 插入到 hash 表中, 返回插入的 hash_node 与 hash->id 的指针偏移量.
static int
hashid_insert(struct hashid * hi, int id) {
	struct hashid_node *c = NULL;
	int i;

	// 从 hash.id 中找到可用的 hash_node
	for (i=0; i < hi->cap; i++) {
		int index = (i+id) % hi->cap;
		if (hi->id[index].id == -1) {
			c = &hi->id[index];
			break;
		}
	}

	// 一定要找到
	assert(c);

	// 数量 +1
	++hi->count;

	// hash_node 记录实际的 id
	c->id = id;

	assert(c->next == NULL);

	// 在 hash.hash 中保存 hash_node
	int h = id & hi->hashmod;
	if (hi->hash[h]) {
		c->next = hi->hash[h];
	}
	hi->hash[h] = c;
	
	return c - hi->id;
}

/// 判断当前 hash 表是否满了
static inline int
hashid_full(struct hashid *hi) {
	return hi->count == hi->cap;
}

#endif
