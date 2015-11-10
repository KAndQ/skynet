#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"
#include "atomic.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128			// socker_server 存储一些信息数据分配的内存空间
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16			// 决定能够管理的 socket 数量, 直接控制当前 skynet 节点能够操作的 socket 数量
#define MAX_EVENT 64			// 每次从 event pool 中读取 event 的最大数量
#define MIN_READ_BUFFER 64		// 初始化 socket 读取数据的最小字节数

// socket 的状态
/*
状态切换的分析:

- invalid 状态的才能被 reserve;

- 处于 invalid 状态的 socket 基本不能做任何的操作, 除了等着被 reserve;

- 在创建侦听 socket 的时候, 状态变为 plisten, 通过调用 start_socket 方法状态改变为 listen, 这时才能获得连接的客户端;

- 在接收到新的客户端连接时, 连接的客户端 socket 状态首先是 paccept, 通过调用 start_socket 方法状态改变为 connected, 这时才能接收/发送数据;

- 在与其他主机连接的时候, 如果直接连接成功则状态直接设置为 connected, 否则设置为 connecting, 侦听事件通知, 如果对应的 socket 有读/写事件, 那么马上设置状态为 connected

- 如果不是主动关闭 socket, 例如是客户端关闭 socket, 那么这时 socket 的状态是 invalid; 如果是主动关闭 socket, 
那么会首先将状态设置为 halfclose, 这个状态会保证先将数据全部发送出去再关闭, 但是在这个状态的 socket 将忽略掉接收的数据, 
也不再发送 close 之后的数据, 只是保证把 close 之前的数据发送出去.

- bind 状态比较特殊, 使用 bind_socket(socket_server_bind) 直接设置状态为 bind 状态
*/
#define SOCKET_TYPE_INVALID 0		// 为使用的 socket 标记, 只有 socket 在这个状态才能被 reserve
#define SOCKET_TYPE_RESERVE 1		// 表示当前 socket 已经有人预约了, 在这个状态的 socket 才能 new_fd(生成 socket)
#define SOCKET_TYPE_PLISTEN 2		// 正在侦听客户端发起的连接, 但是还没有被 start_socket 调用, 这时还无法接收客户端的连接
#define SOCKET_TYPE_LISTEN 3		// 这时才是真正的可以 accpet, 对连接的客户端生成对应 socket
#define SOCKET_TYPE_CONNECTING 4	// socket 正在连接状态, 会将 socket fd 添加到 event pool 中, 根据事件再判断是否连接
#define SOCKET_TYPE_CONNECTED 5		// socket 已连接状态
#define SOCKET_TYPE_HALFCLOSE 6		// 当前的请求主动关闭 socket, 可以认为当前 socket 正在关闭.
#define SOCKET_TYPE_PACCEPT 7		// 当有新的 socket 接入的时候, 新接入的 socket 标记为这个状态, 表示刚刚连接
#define SOCKET_TYPE_BIND 8			// 当前用于 bind 的 socket, socket 服务端的操作流程 bind -> listen -> accept

// 最大的 socket 连接数
#define MAX_SOCKET (1<<MAX_SOCKET_P)

// 数据发送优先级
#define PRIORITY_HIGH 0		// 高
#define PRIORITY_LOW 1		// 低

// id 和 MAX_SOCKET 做 hash 运算
#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)

#define PROTOCOL_TCP 0		// tcp 协议, ipv4
#define PROTOCOL_UDP 1		// udp 协议, ipv4
#define PROTOCOL_UDPv6 2	// udp 协议, ipv6

#define UDP_ADDRESS_SIZE 19	// ipv6 128bit + port 16bit + 1 byte type, udp 地址信息分配内存空间

#define MAX_UDP_PACKAGE 65535	// udp 数据包的大小

// 写数据的缓存, 这是一个链表
struct write_buffer {
	struct write_buffer * next;	// 关联的下一个 write_buffer
	void *buffer;				// 数据的起始地址
	char *ptr;					// 剩余发送数据的起始地址, 这里有个小细节, ptr 是 char * 类型, 指针每次的变化是 1 个字节. 可以参考 send_list_tcp 函数
	int sz;						// 剩余发送数据的大小
	bool userobject;			// 判断 buffer 是否是用户对象, 用户对象的内存控制由 socker_server 的 (soi)socket_object_interface 来决定

	// udp 地址信息, 0 字节存储协议类型; 1, 2 字节存储端口号; 剩下的是地址数据, 对于 IPv4 使用 4 个字节存储, 对于 IPv6 使用 16 个字节存储;
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};

// size_t offsetof(structName, memberName);
// 该宏用于求结构体中一个成员在该结构体中的偏移量.
// 第一个参数是结构体的名字，第二个参数是结构体成员的名字。
// 该宏返回结构体structName中成员memberName的偏移量。偏移量是size_t类型的。

// 得到 tcp 写缓存 数据结构的大小
#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))

// 得到 udp 写缓存 数据结构的大小
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))

// 写数据的链表数据结构
struct wb_list {
	struct write_buffer * head;		// 链表的首指针
	struct write_buffer * tail;		// 链表的尾指针
};

struct socket {
	uintptr_t opaque;		// 不透明的功能作用, 目前在 skynet_socket 中当作 skynet_context 的 handle 使用
	struct wb_list high;	// 写缓存数据的高优先级链表
	struct wb_list low;		// 写缓存数据的低优先级链表
	int64_t wb_size;		// 写数据的总大小, high + low
	int fd;					// 关联的 socket fd
	int id;					// 在 socket_server 中的 id
	uint16_t protocol;		// socket 支持的协议类型, PROTOCOL_TCP, PROTOCOL_UDP, PROTOCOL_UDPv6
	uint16_t type;			// 当前这个 socket 所在的状态
	union {
		int size;			// tcp 情况, read 数据的大小
		uint8_t udp_address[UDP_ADDRESS_SIZE];	// udp 情况下, 存储的是 udp 的地址信息
	} p;
};

// socket_server 服务对象
struct socket_server {
	// 用管道来保证多线程操作, 所有的操作命令通过管道存储起来,
	// 通信线程再从管道将所有的操作命令读取出来.
	int recvctrl_fd;		// pipe 函数的读取端
	int sendctrl_fd;		// pipe 函数的写入端
	int checkctrl;			// 标记是否需要操作管道

	poll_fd event_fd;		// event pool 的文件描述符
	int alloc_id;			// 分配的 id 计数
	int event_n;			// 实际从 event pool 中读取数据的数量
	int event_index;		// 当前处理到的 event 索引
	struct socket_object_interface soi;	// 用户数据类型的内存操作接口
	struct event ev[MAX_EVENT];			// 从 event poll 得到事件的集合
	struct socket slot[MAX_SOCKET];		// 连接的 socket 集合
	char buffer[MAX_INFO];				// 存储一些信息内容, 一般存储 IP 地址信息
	uint8_t udpbuffer[MAX_UDP_PACKAGE];	// 接收到的 udp 数据内容
	fd_set rfds;						// select 函数中判断是否有可读字符集
};

struct request_open {
	int id;		// socket id
	int port;	// 端口
	uintptr_t opaque;
	char host[1];	// 主机名或者地址(IPv4的点分十进制串或者IPv6的16进制串)的字符串起始地址
};

/// 基于 tcp 协议发送数据
struct request_send {
	int id;		// socket id
	int sz;		// 发送数据大小
	char * buffer;	// 发送数据地址
};

/// 基于 udp 协议发送数据
struct request_send_udp {
	struct request_send send;
	uint8_t address[UDP_ADDRESS_SIZE];		// udp 的地址信息
};

/// 设置基于 udp 协议的 socket 地址信息
struct request_setudp {
	int id;	// socket id
	uint8_t address[UDP_ADDRESS_SIZE];		// udp 的地址信息
};

/// 关闭 socket
struct request_close {
	int id;	// socket id
	uintptr_t opaque;
};

struct request_listen {
	int id;	// socket id
	int fd;	// socket fd
	uintptr_t opaque;
	char host[1];	// 主机名或者地址(IPv4的点分十进制串或者IPv6的16进制串)的字符串起始地址
};

struct request_bind {
	int id;	// socket id
	int fd;	// socket fd
	uintptr_t opaque;
};

struct request_start {
	int id;	// socket id
	uintptr_t opaque;
};

/// 基于 IPPROTO_TCP level, 设置 socket 的选项
struct request_setopt {
	int id;	// socket id

	// setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
	int what;	// 设置的选项
	int value;	// 选项值
};

/// 生成基于 udp 协议的 socket
struct request_udp {
	int id;	// socket id
	int fd;	// socket fd
	int family;	// 协议族类型, AF_INET/AF_INET6
	uintptr_t opaque;
};

/*
	The first byte is TYPE
	第一个字节是类型

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
 */

/// 这里也是一个很屌的处理, 每个 request_package 变量, 所占的内存空间是连续的 8 + 256 + 256 = 520 字节大小
struct request_package {
	uint8_t header[8];	// 6 bytes dummy, 前 6 个字节没有使用
	union {
		char buffer[256];		// 这个 buffer 其实不会直接使用, 为的是保证分配的内存空间足够 256 大小
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	} u;
	uint8_t dummy[256];	// 这是一个虚拟的内存空间, 预留使用, 例如: 可以给 request_open.host 用来存储字符串
};

/// 是一个方便 sockaddr 操作的整合功能, 因为内部的成员是共享内存空间的, 这个方式有点屌!!!
union sockaddr_all {
	// 用于存储参与（IP）套接字通信的计算机上的一个internet协议（IP）地址。
	// 为了统一地址结构的表示方法 ，统一接口函数，使得不同的地址结构可以被bind()、connect()、recvfrom()、sendto()等函数调用。
	// 但一般的编程中并不直接对此数据结构进行操作，而使用另一个与之等价的数据结构sockaddr_in, 两者大小都是16字节，所以二者之间可以进行切换。
	struct sockaddr s;

	// 此数据结构用做bind、connect、recvfrom、sendto等函数的参数，指明地址信息。
	// 但一般编程中并不直接针对此数据结构操作，而是使用另一个与sockaddr等价的数据结构.
	struct sockaddr_in v4;

	// 同上, 但是是 ipv6 的协议.
	struct sockaddr_in6 v6;
};

// 发送数据对象
struct send_object {
	void * buffer;	// 数据的指针
	int sz;			// 数据的大小

	// 释放资源的函数接口声明, 释放的是 buffer
	void (*free_func)(void *);
};

// memory macro utils
#define MALLOC skynet_malloc
#define FREE skynet_free

/**
 * 初始化 send_object 结构体
 * @param ss socket_server 结构体
 * @param so send_object 结构体, 待初始化的数据
 * @param object 数据指针
 * @param sz 数据大小. 如果小于 1 则使用 socket_object_interface 初始化 send_object; 否则直接赋值初始化.
 * @return 使用 socket_object_interface 初始化返回 true, 否则返回 false
 */
static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, void *object, int sz) {
	
	// 使用 socket_server 的 socket_object_interface 初始化
	if (sz < 0) {
		so->buffer = ss->soi.buffer(object);
		so->sz = ss->soi.size(object);
		so->free_func = ss->soi.free;
		return true;

	// 直接赋值
	} else {
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	}
}

/// 释放 1 个 write_buffer 内存资源, 会根据 wb->userobject 的值选择不同的释放方式.
static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}

/// 对 fd 对应的 sock 开启 keepalive 功能.
static void
socket_keepalive(int fd) {
	int keepalive = 1;

	// http://baike.baidu.com/link?url=VOPva4krboHp01brpVHJXqS226onNoCSHcfz96UtIzPYCmPd0JGsXvEUkHQ1CSoJ4MLWTP11ykk4ngr7sR0JRq
	// int setsockopt(int sockfd, int level, int optname,const void *optval, socklen_t optlen);
	// 用于任意类型、任意状态套接口的设置选项值。尽管在不同协议层上存在选项，但本函数仅定义了最高的“套接口”层次上的选项。
	// sockfd：标识一个套接口的描述字。
	// level：标识了选项应用的协议. 如果选项是通用的套接字层次选项, 则 level 设置成 SOL_SOCKET, 否则, level 设置成控制这个选项的协议编号, 对于 TCP 选项, 
	//	level 是 IPPROTO_TCP, 对于 IP, level 是 IPPROTO_IP/IPPROTO_IPV6. 
	// optname：需设置的选项。
	// optval：指针，指向存放选项待设置的新值的缓冲区。
	// optlen：optval缓冲区长度。

	// level 定义的层次: 支持 SOL_SOCKET, IPPROTO_TCP, IPPROTO_IP 和 IPPROTO_IPV6。
	// 各个层次的选项值请参考: http://wenku.baidu.com/link?url=tiQDv_PRHTuKYeKgHoAMWvAfw7WQfOxtma2F6kHob2chvb77UNIVkNZKPmWoiDyPtfTSS4uNKSNywGRCYCNwRVOQHR_2Kmm221b4dgE8utO
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));
}

/// 当 socket 的 type 是 INVALID 的时候, 将该 socket.type 设置为 SOCKET_TYPE_RESERVE, 并返回该 id. 如果没有可用的 id, 返回 -1
static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = ATOM_INC(&(ss->alloc_id));
		if (id < 0) {
			// 进行 & 位操作, 将第 32 位符号位清除
			id = ATOM_AND(&(ss->alloc_id), 0x7fffffff);
		}

		// 获得对应的 socket
		struct socket *s = &ss->slot[HASH_ID(id)];
		if (s->type == SOCKET_TYPE_INVALID) {
			if (ATOM_CAS(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				s->id = id;
				s->fd = -1;
				return id;
			} else {	// 如果其他线程使用了该 socket, 从当前的索引开始重新遍历
				// retry
				--i;
			}
		}
	}
	return -1;
}

/// 清空 wb_list 链表
static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}

struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];

	// 创建 poll
	poll_fd efd = sp_create();
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}

	// http://www.cnblogs.com/kunhu/p/3608109.html
	// int pipe(int filedes[2]);
	// 函数说明： pipe()会建立管道，并将文件描述词由参数filedes数组返回, 用于进程间通信.
	// 		filedes[0]为管道里的读取端
	// 		filedes[1]则为管道的写入端
	// 返回值：若成功则返回 0，否则返回-1，错误原因存于errno中。
	// 因为后面没有调用 fork() 函数, 所以 fd[1] >>>> fd[0]
	if (pipe(fd)) {
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}

	// 将读的文件描述符添加到 event pool 中
	// 添加到 event pool 中之后, 只要该文件操作可读, 那么当前线程也不会阻塞
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	// 分配 socket_server 的内存资源
	struct socket_server *ss = MALLOC(sizeof(*ss));

	ss->event_fd = efd;
	ss->recvctrl_fd = fd[0];
	ss->sendctrl_fd = fd[1];
	ss->checkctrl = 1;
	FD_ZERO(&ss->rfds);
	assert(ss->recvctrl_fd < FD_SETSIZE);	// 保证文件描述符没有超过 FD_SETSIZE 大小, 

	// struct socket 初始化
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
	}
	ss->alloc_id = 0;

	ss->event_n = 0;
	ss->event_index = 0;

	memset(&ss->soi, 0, sizeof(ss->soi));

	return ss;
}

/// 释放 wb_list 中元素的资源, 并且将 wb_list 设置为空链表.
static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

/// 强行关闭 socket, 清除其资源
static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}

	// 保证该 ID 不是已经标记为保留的
	assert(s->type != SOCKET_TYPE_RESERVE);

	// 释放链表资源
	free_wb_list(ss, &s->high);
	free_wb_list(ss, &s->low);

	// 用于 accpet 和 listen 的 socket 不会从 event pool 中移除, 因为这时并还没有将 fd 添加到 event pool 中
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {
		sp_del(ss->event_fd, s->fd);
	}

	// 用于 bind 的 socket 不会被 close
	if (s->type != SOCKET_TYPE_BIND) {
		close(s->fd);
	}

	// 标记 socket 是无效的
	s->type = SOCKET_TYPE_INVALID;
}

void 
socket_server_release(struct socket_server *ss) {

	// 释放掉所有的 socket 资源
	int i;
	struct socket_message dummy;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s , &dummy);
		}
	}
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);
	sp_release(ss->event_fd);
	FREE(ss);
}

/// 检查 wb_list 必须是空的链表
static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

/**
 * 创建新的 socket
 * @param ss socket_server
 * @param id 在 socket_server 中可使用的 id
 * @param fd sock 的 fd
 * @param protocol sock 的协议类型
 * @param opaque 
 * @param add 是否将 fd 注册到 event pool 中
 * @return 创建成功返回 struct socket 的指针, 否则返回 NULL
 */
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];

	// id 对应的 socket 必须是已经保留的
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (add) {
		// 保证注册到 event pool 中
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;
	s->protocol = protocol;
	s->p.size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	check_wb_list(&s->high);
	check_wb_list(&s->low);
	return s;
}

// 根据 request_open 提供的信息, 连接到指定的主机, 该连接产生的 sock fd 用于生成一个新的 struct socket.
// 这个函数目前在我看来, 是用在与服务器各个 skynet 节点的连接.
// return -1 when connecting
// 当正在连接的时候返回 -1. 连接成功返回 SOCKET_OPEN, 失败返回 SOCKET_ERROR
static int
open_socket(struct socket_server * ss, struct request_open * request, struct socket_message * result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;

	struct socket *ns;
	int status;

	/*
	typedef struct addrinfo {
	    int ai_flags;			// AI_PASSIVE, AI_CANONNAME, AI_NUMERICHOST
	    int ai_family;			// AF_INET, AF_INET6
	    int ai_socktype;		// SOCK_STREAM, SOCK_DGRAM
	    int ai_protocol;		// IPPROTO_IP, IPPROTO_IPV4, IPPROTO_IPV6, IPPROTO_UDP, IPPROTO_TCP
	    size_t ai_addrlen;		// must be zero or a null pointer
	    char* ai_canonname;		// must be zero or a null pointer
	    struct sockaddr* ai_addr;	// must be zero or a null pointer
	    struct addrinfo* ai_next;	// must be zero or a null pointer
	}
	*/
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	
	// 获得端口号
	char port[16];
	sprintf(port, "%d", request->port);
	memset(&ai_hints, 0, sizeof(ai_hints));

	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	// int getaddrinfo(const char *hostname, const char *service, const struct addrinfo *hints, struct addrinfo **result);
	// hostname:一个主机名或者地址串(IPv4的点分十进制串或者IPv6的16进制串)
	// service：服务名可以是十进制的端口号，也可以是已定义的服务名称，如ftp、http等
	// hints：可以是一个空指针，也可以是一个指向某个addrinfo结构体的指针，调用者在这个结构中填入关于期望返回的信息类型的暗示。
	// result：本函数通过result指针参数返回一个指向addrinfo结构体链表的指针。
	// 返回值: 0 成功, 非 0 出错
	status = getaddrinfo(request->host, port, &ai_hints, &ai_list);
	if (status != 0) {
		result->data = (void *)gai_strerror(status);
		goto _failed;
	}

	int sock = -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
		// int socket(int domain, int type, int protocol);
		// 返回值: 若成功, 返回文件(套接字)描述符; 若出错, 返回 -1
		sock = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
		if (sock < 0) {
			continue;
		}

		// 开启 keepalive 功能
		socket_keepalive(sock);

		// 设置当前 sock 非阻塞
		sp_nonblocking(sock);

		// 将 sock 连接到指定的服务器
		// int connect(int sockfd, const struct sockaddr * addr, socklen_t len);
		// 返回值: 若成功, 返回 0, 若出错, 返回 -1
		status = connect(sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if (status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;
			continue;
		}

		// 只要有一个 sock 连接成功则跳出循环
		break;
	}

	if (sock < 0) {
		result->data = strerror(errno);
		goto _failed;
	}

	// 利用生成的 sock 文件描述符创建 struct socket, 注意这里使用的是 PROTOCOL_TCP
	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	// 连接 request_open 的主机成功
	if(status == 0) {
		// 标记为连接主机成功
		ns->type = SOCKET_TYPE_CONNECTED;

		// 获得主机的 struct sockaddr 数据
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;

		// const char * inet_ntop(int af, const void *src, char *dst, socklen_t cnt);
		// 将 src(二进制数据) 转化为 dst(字符串, 格式为 192.168.1.1)

		// int inet_pton(int af, const char *src, void *dst);
		// 将 src(字符串数据, 格式为 192.168.1.1) 转化为 dst(二进制数据)
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}

		freeaddrinfo(ai_list);
		return SOCKET_OPEN;

	// 连接 request_open 的主机未成功
	} else {
		// 标记为正在连接状态
		ns->type = SOCKET_TYPE_CONNECTING;

		// 对其写状态进行侦听, 等待连接完成, 当连接成功的时候, 会派发写事件, 这时再设置成 SOCKET_TYPE_CONNECTED, 具体查看函数 report_connect.
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	freeaddrinfo(ai_list);
	return -1;

_failed:
	freeaddrinfo(ai_list);

	// 如果失败, 标记该 id 是无效的
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

/// 基于 tcp 协议, 使用 socket 将 wb_list 内的数据发送出去, 但是并不保证会将 wb_list 内的所有数据全部发送出去.
/// 返回值, 返回 -1, 表示发送操作完成; SOCKET_CLOSE, 表示关闭掉了该 socket.
static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	
	// 理想状态下是希望将 wb_list 内的数据全部发送出去
	while (list->head) {
		struct write_buffer * tmp = list->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);

			if (sz < 0) {
				switch(errno) {

				// 关于 EINTR 错误代码的详细解释: http://blog.csdn.net/benkaoya/article/details/17262053
				case EINTR:		// 被信号所中断, 这只是临时性的, 再次调用可能成功, 所以下面使用 continue.
					continue;

				// 关于 EAGAIN 错误代码的详细解释: http://blog.chinaunix.net/uid-20737871-id-1881207.html
				case EAGAIN:	// 此动作会令进程阻断，但参数 s 的 socket 为不可阻断的。缓冲区已满时会报告该错误, 数据将不再发送.
					return -1;
				}

				// 对于其他的错误, 将强行关闭 socket
				force_close(ss, s, result);
				return SOCKET_CLOSE;
			}

			// 减掉已发送数据的大小
			s->wb_size -= sz;

			// 数据并没有完全发送出去, 将已经发送的数据忽略掉, 并且停止继续发送
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		list->head = tmp->next;
		write_buffer_free(ss, tmp);
	}
	list->tail = NULL;

	return -1;
}

/// 将 udp_address 的数据给 sockaddr_all, 返回数据, 操作协议对应结构体的大小. 若没有对应的协议, 返回 0.
static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	// 类型判断, 保证与 socket 的协议类型相同
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;

	// 拿到端口数据
	uint16_t port = 0;
	memcpy(&port, udp_address+1, sizeof(uint16_t));

	switch (s->protocol) {
	case PROTOCOL_UDP:

		// 拿到 IPv4 的地址数据
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));	// ipv4 address is 32 bits
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:

		// 拿到 IPv6 的地址数据
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
		return sizeof(sa->v6);
	}
	return 0;
}

/// 基于 udp 协议, 使用 socket 将 wb_list 的数据发送出去, 但是并不保证会将 wb_list 内的所有数据全部发送出去.
/// 函数始终返回 -1
static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {

	// 理想状态下是希望将 wb_list 数据全部发送出去
	while (list->head) {
		struct write_buffer * tmp = list->head;

		// 获得地址数据
		union sockaddr_all sa;
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);

		// int sendto(socket s, const void * msg, int len, unsigned int flags, const struct sockaddr * to, int tolen);
		// 指向一指定目的地发送数据，适用于发送未建立连接的 UDP 数据报.
		// 成功则返回实际传送出去的字符数，失败返回－1，错误原因存于errno 中。
		int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);

		// 只要产生错误, 那么将停止发送
		if (err < 0) {
			switch(errno) {
			case EINTR:
			case EAGAIN:
				return -1;
			}
			fprintf(stderr, "socket-server : udp (%d) sendto error %s.\n",s->id, strerror(errno));
			return -1;
/*			// ignore udp sendto error
			忽略 udp sendto 函数的错误
			
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = NULL;

			return SOCKET_ERROR;
*/
		}

		s->wb_size -= tmp->sz;
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}

/// 根据 socket.protocol 协议类型选择将 wb_list 数据发送出去的方式
static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	if (s->protocol == PROTOCOL_TCP) {
		return send_list_tcp(ss, s, list, result);
	} else {
		return send_list_udp(ss, s, list, result);
	}
}

/// 判断 wb_list 链表是否完成发送
static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	// 如果 ptr 和 buffer 不相同, 表示当前正在操作
	return (void *)wb->ptr != wb->buffer;
}

/// 
static void
raise_uncomplete(struct socket * s) {

	// 将 low 的链表的第二个元素作为 low 的头
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	// 将之前 low 链表的头移动到空的 high 链表
	struct wb_list *high = &s->high;

	// 保证 high 链表必须为空
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

/*
	Each socket has two write buffer list, high priority and low priority.
	每个 socket 拥有两个写缓存链表, 高优先级的和低优先级的.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. 

	1. 尽可能的发送高优先级的链表数据.
	2. 如果高优先级链表为空, 那么尝试发送低优先级链表数据.
	3. 如果低优先级的链表未完全发送数据(只发送了部分数据), 将低优先级链表的头移动到空的高优先级链表中(调用 raise_uncomplete).
	4. 如果两个链表都是空的, 关闭写事件的侦听. 
 */
/// 将 socket 的 high 和 low 链表内的数据发送出去
/// 返回值, 发送成功返回 -1, 否则返回 SOCKET_CLOSE
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {

	// 必须保证低优先级的链表数据之前已经完全发送成功
	assert(!list_uncomplete(&s->low));

	// step 1
	if (send_list(ss, s, &s->high, result) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}

	// 必须先将高优先级链表的数据发送完
	if (s->high.head == NULL) {
		
		// step 2
		if (s->low.head != NULL) {
			if (send_list(ss, s, &s->low, result) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
			}

			// step 3
			if (list_uncomplete(&s->low)) {
				raise_uncomplete(s);
			}
		} else {

			// step 4
			sp_write(ss->event_fd, s->fd, s, false);

			if (s->type == SOCKET_TYPE_HALFCLOSE) {
				force_close(ss, s, result);
				return SOCKET_CLOSE;
			}
		}
	}

	return -1;
}

/**
 * 将 request_send 的发送数据转化为 write_buffer 添加到 wb_list 链表中
 * @param ss socket_server
 * @param s 添加数据的的 wb_list
 * @param request 数据来源
 * @param size 分配的内存大小, 分配给 write_buffer 的内存大小会因为 tcp 和 udp 协议不同而不同
 * @param n write_buffer ptr 指针相对 buffer 指针的偏移量
 * @return 生成的 write_buffer
 */
static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size, int n) {
	// 分配内存资源
	struct write_buffer * buf = MALLOC(size);

	// 初始化 write_buffer
	struct send_object so;
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	buf->ptr = (char *)so.buffer + n;	// 计算发送数据的偏移量
	buf->sz = so.sz - n;	// 计算剩余发送数据的大小
	buf->buffer = request->buffer;	// 保存发送数据的起始地址
	buf->next = NULL;

	// 将 write_buffer 添加到 wb_list 中
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf;
}

/// 基于 udp 协议, 将 request_send 的数据添加到 socket 的写队列中
static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER, 0);

	// write_buffer 会对 udp_address 的内容进行复制, 复制到自己的 udp_address 中
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
	s->wb_size += buf->sz;
}

/// 基于 tcp 协议, 将 request_send 的数据添加到 socket 的 high 写队列中
static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request, int n) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER, n);
	s->wb_size += buf->sz;
}

/// 基于 tcp 协议, 将 request_send 的数据添加到 socket 的 low 写队列中
static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER, 0);
	s->wb_size += buf->sz;
}

/// 判断当前 socket 的待发送队列(high 和 low)是否为空
static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW
	当一个数据包发送的时候, 我们可以指派优先级: PRIORITY_HIGH 或者 PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.

	如果 socket 写队列为空, 那么直接使用文件描述符(fd)调用 write 方法
		如果写入数据只操作了一部分, 那么将剩余的数据添加到 high 链表中. (即使 priority 参数的优先级是 PRIORITY_LOW)
	否则会把数据包添加到高优先级队列(PRIORITY_HIGH)或者低优先级队列(PRIORITY_LOW)中.
 */
/// 将 request_send 的数据发送出去. 返回值, 发送成功返回 -1, 否则返回 SOCKET_CLOSE
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	struct send_object so;
	send_object_init(ss, &so, request->buffer, request->sz);
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {

		// 看到这里我理解了好几种意思:
		// socket_object_interface.buffer 接口不应该分配新的内存空间
		// socket_object_interface.free_func 能够释放掉分配的内存空间
		so.free_func(request->buffer);
		return -1;
	}

	// 侦听状态的 socket 是不能传输数据的
	if (s->type == SOCKET_TYPE_PLISTEN || s->type == SOCKET_TYPE_LISTEN) {
		fprintf(stderr, "socket-server: write to listen fd %d.\n", id);
		so.free_func(request->buffer);
		return -1;
	}

	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
		if (s->protocol == PROTOCOL_TCP) {
			// tcp
			int n = write(s->fd, so.buffer, so.sz);
			if (n < 0) {
				switch(errno) {
				case EINTR:
				case EAGAIN:
					n = 0;
					break;
				default:
					fprintf(stderr, "socket-server: write to %d (fd=%d) error :%s.\n", id, s->fd, strerror(errno));
					force_close(ss, s, result);
					so.free_func(request->buffer);
					return SOCKET_CLOSE;
				}
			}

			// 如果数据已经完全写入到发送缓存中
			if (n == so.sz) {
				so.free_func(request->buffer);
				return -1;
			}

			// add to high priority list, even priority == PRIORITY_LOW
			// 添加到高优先级链表, 即使 priority == PRIORITY_LOW
			append_sendbuffer(ss, s, request, n);
		} else {
			// udp

			// 如果没有传入 udp 地址, 则使用上次的发送地址
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
			if (n != so.sz) {
				append_sendbuffer_udp(ss, s, priority, request, udp_address);
			} else {
				so.free_func(request->buffer);
				return -1;
			}
		}

		// 达到这里表示还需要继续写数据, 所以开启可写事件侦听
		sp_write(ss->event_fd, s->fd, s, true);

	} else {
		// 链表非空情况下的逻辑处理

		if (s->protocol == PROTOCOL_TCP) {
			if (priority == PRIORITY_LOW) {
				append_sendbuffer_low(ss, s, request);
			} else {
				append_sendbuffer(ss, s, request, 0);
			}
		} else {
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			append_sendbuffer_udp(ss, s, priority, request, udp_address);
		}
	}
	return -1;
}

/// 根据 request_listen 生成新的 socket, 并且将 type 标记为 SOCKET_TYPE_PLISTEN. 成功返回 -1, 否则返回 SOCKET_ERROR
static int
listen_socket(struct socket_server * ss, struct request_listen * request, struct socket_message * result) {
	int id = request->id;
	int listen_fd = request->fd;
	struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;
	return -1;

_failed:
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = "reach skynet socket number limit";
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERROR;
}

/// 根据 request_close 关闭 socket.
/// 返回值, 成功关闭返回 SOCKET_CLOSE, 否则返回 -1
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];

	// 不符合条件直接返回
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}

	// 如果发送链表中还有数据, 需要将数据全部发出
	if (!send_buffer_empty(s)) { 
		int type = send_buffer(ss, s, result);
		if (type != -1)
			return type;
	}

	// 如果发送链表为空了, 那么释放 socket 的资源
	if (send_buffer_empty(s)) {
		force_close(ss, s, result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}

	// 如果还有链表不为空, 那么将 socket 标记为半关闭状态, 直到写链表里面的数据全部发送完之后, 才会真正的关闭该 socket
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}

/// 根据 request_bind 生成 socket, 返回值, 成功返回 SOCKET_OPEN, 失败返回 SOCKET_ERROR.
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = "reach skynet socket number limit";
		return SOCKET_ERROR;
	}

	// 设置 sock 为非阻塞
	sp_nonblocking(request->fd);

	// 标记为 bind
	s->type = SOCKET_TYPE_BIND;
	result->data = "binding";
	return SOCKET_OPEN;
}

/// 根据 request_start 操作 socket, 返回值, 成功返回 SOCKET_OPEN, 失败返回 SOCKET_ERROR, 没做什么处理返回 -1
static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[HASH_ID(id)];

	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->data = "invalid socket";
		return SOCKET_ERROR;
	}

	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {
		if (sp_add(ss->event_fd, s->fd, s)) {	// 添加到 event pool 中!!!
			s->type = SOCKET_TYPE_INVALID;
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;	// 这里很重要, 在 skynet 中是关联到所在的 skynet_context 的 handle
		result->data = "start";
		return SOCKET_OPEN;
	} else if (s->type == SOCKET_TYPE_CONNECTED) {
		s->opaque = request->opaque;	// 这里很重要, 在 skynet 中是关联到所在的 skynet_context 的 handle
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	return -1;
}

/// 根据 request_setopt 操作 socket
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		return;
	}
	int v = request->value;
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}

/// 以阻塞的方式, 从管道读出数据
static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {
		int n = read(pipefd, buffer, sz);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "socket-server : read pipe error %s.\n",strerror(errno));
			return;
		}

		// must atomic read from a pipe
		// 保证从管道读取数据必须是 1 次原子操作
		assert(n == sz);

		return;
	}
}

/// 判断 ss->recvctrl_fd 是否有数据可读, 有数据可读返回 1, 否则返回 0.
static int
has_cmd(struct socket_server *ss) {

	// 不等待, 立即返回
	struct timeval tv = {0, 0};
	int retval;

	// 添加到描述符集里面
	FD_SET(ss->recvctrl_fd, &ss->rfds);

	// 侦测是否可读
	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);
	if (retval == 1) {
		return 1;
	}
	return 0;
}

/// 根据 request_udp 生成用于 udp 通信的 socket
static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	int id = udp->id;

	int protocol;
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}

	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		close(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return;
	}

	ns->type = SOCKET_TYPE_CONNECTED;
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

/// 根据 request_setudp 设置 socket 的 udp 地址信息. 返回值, 成功返回 -1, 失败返回 SOCKET_ERROR
static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return -1;
	}

	int type = request->address[0];
	if (type != s->protocol) {
		// protocol mismatch
		// 协议匹配出错
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		result->data = "protocol mismatch";

		return SOCKET_ERROR;
	}

	if (type == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	return -1;
}

// return type
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;
	
	// the length of message is one byte, so 256+8 buffer size is enough.
	// 消息长度是 1 个字节, 所以 256+8 缓存大小足够了.

	uint8_t buffer[256];
	uint8_t header[2];

	// 读取命令类型和数据长度
	block_readpipe(fd, header, sizeof(header));
	int type = header[0];
	int len = header[1];

	// 读取实际的数据内容
	block_readpipe(fd, buffer, len);

	// ctrl command only exist in local fd, so don't worry about endian.
	// 控制命令只是存在于本地的 fd, 所以不用担心字节存储次序.

	switch (type) {
	case 'S':
		// 返回 SOCKET_OPEN, SOCKET_ERROR, -1
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		// 返回 SOCKET_OPEN, SOCKET_ERROR
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
		// 返回 SOCKET_ERROR, -1
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		// 返回 SOCKET_CLOSE, -1 
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':
		// 返回 SOCKET_OPEN, SOCKET_ERROR, -1
		return open_socket(ss, (struct request_open *)buffer, result);
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
		// 返回 SOCKET_CLOSE, -1
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_HIGH, NULL);
	case 'P':
		// 返回 SOCKET_CLOSE, -1
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_LOW, NULL);
	case 'A': {
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		// 返回 SOCKET_CLOSE, -1
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}
	case 'C':
		// 返回 SOCKET_ERROR, -1 
		return set_udp_address(ss, (struct request_setudp *)buffer, result);
	case 'T':
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
	case 'U':
		add_udp_socket(ss, (struct request_udp *)buffer);
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
/// 基于 tcp 协议, 读取数据成功返回 SOCKET_DATA
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_message * result) {

	// socket 读取数据
	int sz = s->p.size;
	char * buffer = MALLOC(sz);		// 在这里分配读取数据的内存
	int n = (int)read(s->fd, buffer, sz);

	// 错误处理
	if (n < 0) {
		// 释放分配的内存资源
		FREE(buffer);

		switch(errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			// 其他错误将直接关闭掉 socket
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}

		return -1;
	}

	// 因为是基于 event pool 来读取数据的, 即保证了有数据可读, 可是这时读取的数据确为 0, 那么则认为该 socket 已经关闭了.
	if (n == 0) {
		FREE(buffer);
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	// 如果当前的 socket 是 SOCKET_TYPE_HALFCLOSE 状态, 忽略掉读取的消息
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		// 忽略掉接收的数据
		FREE(buffer);
		return -1;
	}

	// 动态调整每次请求从缓存中读取字节数量

	if (n == sz) {
		// 如果能够完整读取出数据, 那么加大请求读取的字节数量
		s->p.size *= 2;
	} else if (sz > MIN_READ_BUFFER && n * 2 < sz /* 可以理解为 n 就是扩大了两倍 sz 都比你大, 所以 sz 缩小两倍定大于 n */) {
		// 如果缓存中并没有那么多的数据可读, 那么减小请求读取的字节数量
		s->p.size /= 2;
	}

	// 记录这次操作的结果
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;

	return SOCKET_DATA;
}

/// 类似 udp_socket_address 函数, 根据 protocol 使用 sockaddr_all 初始化 udp_address. 返回协议对应结构体的大小.
static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;
	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address + addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsz += sizeof(sa->v4.sin_port);
		memcpy(udp_address + addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsz += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address + addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsz += sizeof(sa->v6.sin6_port);
		memcpy(udp_address + addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsz += sizeof(sa->v6.sin6_addr);
	}
	return addrsz;
}

/// 基于 udp 协议, 读取数据, 读取成功返回 SOCKET_UDP
static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_message * result) {

	// 读取数据
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);

	// ssize_t recvfrom(int sockfd,void *buf,int len,unsigned int flags, struct sockaddr *from,socket_t *fromlen); 
	// 接收一个数据报并保存源地址。
	// s：标识一个已连接套接口的描述字。
	// buf：接收数据缓冲区。
	// len：缓冲区长度。
	// flags：调用操作方式。
	// from：（可选）指针，指向装有源地址的缓冲区。
	// fromlen：（可选）指针，指向from缓冲区长度值。
	// 返回值, 若无错误发生，recvfrom()返回读入的字节数。如果连接已中止，返回0。否则的话，返回错误代码
	int n = recvfrom(s->fd, ss->udpbuffer, MAX_UDP_PACKAGE, 0, &sa.s, &slen);

	// 错误处理
	if (n < 0) {
		switch(errno) {
		case EINTR:
		case EAGAIN:
			break;
		default:
			// close when error
			// 其他错误将关闭掉这个 socket
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		return -1;
	}

	// 将地址信息存储在 data + n 的内存空间位置
	uint8_t * data;
	if (slen == sizeof(sa.v4)) {
		if (s->protocol != PROTOCOL_UDP)
			return -1;
		data = MALLOC(n + 1 + 2 + 4);
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (s->protocol != PROTOCOL_UDPv6)
			return -1;
		data = MALLOC(n + 1 + 2 + 16);
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}

	// 复制读取的数据内容
	memcpy(data, ss->udpbuffer, n);

	// 记录操作结果
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = (char *)data;

	return SOCKET_UDP;
}

/// 报告 socket 连接, 成功返回 SOCKET_OPEN, 否则返回 SOCKET_ERROR
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);

	// 如果 socket 出错, 那么将关闭掉该 socket
	if (code < 0 || error) {  
		force_close(ss,s, result);
		if (code >= 0)
			result->data = strerror(error);
		else
			result->data = strerror(errno);
		return SOCKET_ERROR;

	// 正确逻辑处理
	} else {

		// 标记 socket 为 SOCKET_TYPE_CONNECTED 状态
		s->type = SOCKET_TYPE_CONNECTED;

		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;

		// 如果写缓存链表为空, 那么禁用掉侦听写事件
		if (send_buffer_empty(s)) {
			sp_write(ss->event_fd, s->fd, s, false);
		}

		union sockaddr_all u;
		socklen_t slen = sizeof(u);

		// int getpeername(int s, struct sockaddr *name, socklen_t *namelen);
		// 获取与套接口相连的端地址。
		// s：标识一已连接套接口的描述字。
		// name：接收端地址的名字结构。
		// namelen：返回名字结构的长度。
		// 返回值, 成功返回 0, 否则返回错误代码

		/*
		getsockname和getpeername调度时机很重要，如果调用时机不对，则无法正确获得地址和端口。
		
		TCP
		对于服务器来说，在bind以后就可以调用getsockname来获取本地地址和端口，虽然这没有什么太多的意义。
		getpeername只有在链接建立以后才调用，否则不能正确获得对方地址和端口，所以他的参数描述字一般是链接描述字而非监听套接口描述字。

		对于客户端来说，在调用socket时候内核还不会分配IP和端口，此时调用getsockname不会获得正确的端口和地址（当然链接没建立更不可能调用getpeername），
		当然如果调用了bind 以后可以使用getsockname。想要正确的到对方地址（一般客户端不需要这个功能），则必须在链接建立以后，同样链接建立以后，
		此时客户端地址和端口就已经被指定，此时是调用getpeername的时机。

		UDP
		UDP分为链接和没有链接2种

		没有链接的UDP不能调用getpeername，但是可以调用getsockname, 和TCP一样，他的地址和端口不是在调用socket就指定，而是在第一次调用sendto函数以后.

		已经链接的UDP，在调用connect以后，这2个函数都是可以用的（同样，getpeername也没太大意义。如果你不知道对方的地址和端口，不可能会调用connect）。
		*/

		if (getpeername(s->fd, &u.s, &slen) == 0) {
			// 得到被连接端的地址信息, 用字符串存储起来
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;

		// 即使 getpeername 会出错, 但是不能保证这个 socket 未连接, 所以还是认为连接成功.
		// 在后面的逻辑中再去处理 socket 是否连接的问题.
		return SOCKET_OPEN;
	}
}

// return 0 when failed, or -1 when file limit
// 失败的时候返回 0, 或者当到达文件限制时返回 -1

/// 通过 accpet 得到连接的 socket, 成功返回 1, 否则返回 0
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {

	// 存储连接端的地址信息
	union sockaddr_all u;
	socklen_t len = sizeof(u);

	// int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
	// 在一个套接口接受一个连接。
	// sockfd：套接口描述字，该套接口在 listen() 后监听连接。
	// addr：（可选）指针，指向一缓冲区，其中接收为通讯层所知的连接实体的地址。Addr参数的实际格式由套接口创建时所产生的地址族确定。
	// addrlen：（可选）指针，输入参数，配合addr一起使用，指向存有addr地址长度的整型数。
	// 如果 addr 与 addrlen 中有一个为 0/NULL，将不返回所接受的套接口远程地址的任何信息。
	// 返回值, 如果成功返回连接成功的 socket 的文件描述符, 否则返回错误代码
	int client_fd = accept(s->fd, &u.s, &len);

	// 如果没有连接的 socket 函数直接返回
	if (client_fd < 0) {
		if (errno == EMFILE || errno == ENFILE) {
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = strerror(errno);
			return -1;
		} else {
			return 0;
		}
	}

	// 拿到一个可用的 socket id, 该 socket 已经标记为 SOCKET_TYPE_RESERVE
	int id = reserve_id(ss);

	// 如果没有可用的 id, 那么将关闭掉连接的 socket, 并且函数直接返回
	if (id < 0) {
		close(client_fd);
		return 0;
	}

	// 开启保持活动检测
	socket_keepalive(client_fd);

	// 连接的 socket 是非阻塞的方式工作
	sp_nonblocking(client_fd);

	// 使用之前保留的 id, 生成一个 socket, 注意, 这时不会注册到 event pool 中.
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}

	// 标记为 SOCKET_TYPE_PACCEPT 状态
	ns->type = SOCKET_TYPE_PACCEPT;

	// 记录操作结果
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;	// 在 paccept 时, ud 保存的是 id
	result->data = NULL;

	// 将连接端的地址信息转化为字符串, 并存储起来
	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);
		result->data = ss->buffer;
	}

	return 1;
}

/// 对于操作产生 SOCKET_CLOSE 和 SOCKET_ERROR 的 socket, 在 event pool 中查询到关联的 event, 将 event 的 s 设置为 NULL.
/// 这样 event 不论是可读还是可写, 都无法再操作 socket. 当然, 前提是该 socket 的状态也已经是 SOCKET_TYPE_INVALID
static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {
	if (type == SOCKET_CLOSE || type == SOCKET_ERROR) {
		int id = result->id;
		int i;
		for (i=ss->event_index; i<ss->event_n; i++) {
			struct event *e = &ss->ev[i];
			struct socket *s = e->s;
			if (s) {
				if (s->type == SOCKET_TYPE_INVALID && s->id == id) {
					e->s = NULL;
					break;
				}
			}
		}
	}
}

// return type
// 返回类型
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {

		if (ss->checkctrl) {
			// 如果有数据可读
			if (has_cmd(ss)) {
				// 执行命令, 返回的是 -1 表明需要继续处理后续的命令
				int type = ctrl_cmd(ss, result);
				if (type != -1) {
					// 处理 SOCKET_CLOSE 和 SOCKET_ERROR 的 socket
					clear_closed_event(ss, result, type);
					return type;
				} else
					continue;

			// 没有命令可读的时候设置标记
			} else {
				ss->checkctrl = 0;
			}
		}

		// 当事件处理完的时候, 从新获取新的时间集合
		if (ss->event_index == ss->event_n) {
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			ss->checkctrl = 1;	// 标记为需要从管道读取数据

			// 告诉通信线程, 这次不需要处理条件信号
			if (more) {
				*more = 0;
			}

			ss->event_index = 0;

			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}

		// 获得一个 event
		struct event *e = &ss->ev[ss->event_index++];
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			// 在开始的时候分派管道的消息
			continue;
		}

		// 根据 socket 的 type 做逻辑处理

		switch (s->type) {
		case SOCKET_TYPE_CONNECTING:	// 这个在之前也介绍过(open_socket), 正在连接主机, 这时接收到可读或者可写事件的时候则认为已经连接成功了
			// 返回 SOCKET_ERROR, SOCKET_OPEN
			return report_connect(ss, s, result);
		case SOCKET_TYPE_LISTEN: {		// 此时这个 socket 正在 listen
			int ok = report_accept(ss, s, result);
			if (ok > 0) {
				return SOCKET_ACCEPT;
			} if (ok < 0 ) {
				return SOCKET_ERROR;
			}
			// when ok == 0, retry
			// 当 ok == 0, 重试
			break;
		}
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:
			// 如果可读
			if (e->read) {
				int type;
				if (s->protocol == PROTOCOL_TCP) {
					// 返回 SOCKET_ERROR, SOCKET_CLOSE, SOCKET_DATA, -1
					type = forward_message_tcp(ss, s, result);
				} else {
					// 返回 SOCKET_ERROR, SOCKET_UDP, -1 
					type = forward_message_udp(ss, s, result);
					if (type == SOCKET_UDP) {
						// try read again
						// 尝试再读一次
						--ss->event_index;

						return SOCKET_UDP;
					}
				}

				if (e->write && type != SOCKET_CLOSE && type != SOCKET_ERROR) {
					// Try to dispatch write message next step if write flag set.
					// 如果 write 为 true, 那么尝试在在下次循环的时候调度写信息操作
					e->read = false;
					--ss->event_index;
				}

				if (type == -1)
					break;

				return type;
			}

			// 如果可写
			if (e->write) {
				// 发送数据
				int type = send_buffer(ss, s, result);
				
				if (type == -1)
					break;
				
				return type;
			}
			break;
		}
	}
}

/// 将 type 和 len 数据写入到管道中
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;

	// 必须保证数据写入
	for (;;) {
		int n = write(ss->sendctrl_fd, &request->header[6], len+2);

		if (n < 0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}

		// 保证数据完全写入
		assert(n == len+2);
		return;
	}
}

/// 初始化一个 request_open, 数据赋值给 req.u.open
/// 返回值, 成功返回主机地址的(addr)的字符串长度, 失败返回 -1
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {

	// 主机地址验证
	int len = strlen(addr);
	if (len + sizeof(req->u.open) >= 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return -1;
	}

	// 申请一个可用的 socket id
	int id = reserve_id(ss);
	if (id < 0)
		return -1;

	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;

	// 因为 request_package 分配的内存空间足够大, 所以这样处理是没有问题的, 尽管 open.host 表示的是 1 个大小的 char 数组,
	// 但是 request_package 为 u.open.host 预留了足够的连续内存空间, 所以可以直接 memcpy 而不用担心内存覆盖问题. 
	// 其实也就是把 addr 的数据放入到 request_package 的内存空间中.
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	// 生成一个 request_open
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);
	if (len < 0)
		return -1;

	// 将 request_open 的数据写入到管道中
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}

/// 释放 buffer 资源
static void
free_buffer(struct socket_server *ss, const void * buffer, int sz) {
	struct send_object so;
	send_object_init(ss, &so, (void *)buffer, sz);
	so.free_func((void *)buffer);
}

// return -1 when error
// 错误的时候返回 -1
int64_t 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {

	// 有效性校验
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	// 生成一个 request_send
	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	// 将 request_send 写入到管道中
	send_request(ss, &request, 'D', sizeof(request.u.send));

	return s->wb_size;
}

void 
socket_server_send_lowpriority(struct socket_server *ss, int id, const void * buffer, int sz) {

	// 有效性校验
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return;
	}

	// 生成一个 request_send
	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	// 将 request_send 写入到管道中
	send_request(ss, &request, 'P', sizeof(request.u.send));
}

void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	// 生成 request_close
	struct request_package request;
	request.u.close.id = id;
	request.u.close.opaque = opaque;

	// 将 request_close 写入管道中
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

// return -1 means failed
// or return AF_INET or AF_INET6
// 失败时返回 -1, 成功的情况下 参数 family 等于 AF_INET 或者 AF_INET6

/// bind 到指定的 [地址, 端口], 失败返回 -1, 成功得到 bind 成功的 socket 的文件描述符
static int
do_bind(const char *host, int port, int protocol, int *family) {
	int fd;
	int status;
	int reuse = 1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	if (host == NULL || host[0] == 0) {
		host = "0.0.0.0";	// INADDR_ANY, 就是指定地址为0.0.0.0的地址，这个地址事实上表示不确定地址，或“所有地址”、“任意地址”。 一般来说，在各个系统中均定义成为0值。
	}

	// 端口字符串格式化
	sprintf(portstr, "%d", port);

	memset( &ai_hints, 0, sizeof( ai_hints ) );

	ai_hints.ai_family = AF_UNSPEC;

	// 指定协议
	if (protocol == IPPROTO_TCP) {
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;

	// 获取地址信息
	status = getaddrinfo( host, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}

	*family = ai_list->ai_family;

	fd = socket(*family, ai_list->ai_socktype, 0);
	if (fd < 0) {
		goto _failed_fd;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}

	// int bind( int sockfd, const struct sockaddr * my_addr, socklen_t addrlen);
	// 将一本地地址与一套接口捆绑。
	// sockfd 表示已经建立的socket编号（描述符）；
	// my_addr 是一个指向sockaddr结构体类型的指针；
	// addrlen表示my_addr结构的长度，可以用sizeof函数获得
	// 返回：成功返回0，失败返回-1.

	status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);
	if (status != 0)
		goto _failed;

	freeaddrinfo( ai_list );

	return fd;

_failed:
	close(fd);

_failed_fd:
	freeaddrinfo( ai_list );
	return -1;
}

/// 侦听指定的 [地址, 端口], 失败返回 -1, 成功返回正在侦听的 socket 文件描述符
static int
do_listen(const char * host, int port, int backlog) {
	int family = 0;
	int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);
	if (listen_fd < 0) {
		return -1;
	}

	// int listen(SOCKET sockfd, int backlog)
	// listen函数使用主动连接套接字变为被连接套接口，使得一个进程可以接受其它进程的请求，从而成为一个服务器进程。
	// 在TCP服务器编程中listen函数把进程变为一个服务器，并指定相应的套接字变为被动连接。
	// listen函数一般在调用bind之后-调用accept之前调用。
	// sockfd: 一个已绑定未被连接的套接字描述符
	// backlog: backlog 参数定义了等待连接的队列的最大长度. 如果一个连接请求作用在已满的队列上, 客户端可能会收到一个 ECONNREFUSED 错误. 
	// 			另外,如果底层协议支持重传,请求可能会被忽略,这样重试可能成功。
	// 返回：成功返回0，失败返回-1.

	if (listen(listen_fd, backlog) == -1) {
		close(listen_fd);
		return -1;
	}

	return listen_fd;
}

int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	// 开始侦听
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	
	// 申请 ID
	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return id;
	}

	// 生成 request_listen
	struct request_package request;
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;

	// 将 request_listen 写入管道
	send_request(ss, &request, 'L', sizeof(request.u.listen));
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	
	// 申请 socket id
	int id = reserve_id(ss);
	if (id < 0)
		return -1;

	// 生成 request_bind
	struct request_package request;
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;

	// 将 request_bind 写入管道
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	// 生成 request_start
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;

	// 将 request_start 写入管道
	send_request(ss, &request, 'S', sizeof(request.u.start));
}

void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP

int 
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int fd;
	int family;
	if (port != 0 || addr != NULL) {
		// bind
		fd = do_bind(addr, port, IPPROTO_UDP, &family);
		if (fd < 0) {
			return -1;
		}
	} else {
		family = AF_INET;
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}

	// 设置非阻塞模式
	sp_nonblocking(fd);

	// 申请 socket id
	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return -1;
	}

	// 生成 request_udp
	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = fd;
	request.u.udp.opaque = opaque;
	request.u.udp.family = family;

	// 将 request_udp 写入管道
	send_request(ss, &request, 'U', sizeof(request.u.udp));	
	return id;
}

int64_t 
socket_server_udp_send(struct socket_server *ss, int id, const struct socket_udp_address *addr, const void *buffer, int sz) {

	// 有效性校验
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	// 生成 request_send_udp
	struct request_package request;
	request.u.send_udp.send.id = id;
	request.u.send_udp.send.sz = sz;
	request.u.send_udp.send.buffer = (char *)buffer;

	// 复制地址信息
	const uint8_t *udp_address = (const uint8_t *)addr;
	int addrsz;
	switch (udp_address[0]) {
	case PROTOCOL_UDP:
		addrsz = 1+2+4;		// 1 type, 2 port, 4 ipv4
		break;
	case PROTOCOL_UDPv6:
		addrsz = 1+2+16;	// 1 type, 2 port, 16 ipv6
		break;
	default:
		free_buffer(ss, buffer, sz);
		return -1;
	}
	memcpy(request.u.send_udp.address, udp_address, addrsz);	

	// 将 request_send_udp 写入管道
	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);
	return s->wb_size;
}

int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];

	// 拿到目标的地址信息
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;
	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}

	// 生成 request_setudp
	struct request_package request;
	request.u.set_udp.id = id;
	int protocol;

	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo( ai_list );
		return -1;
	}

	int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

	freeaddrinfo( ai_list );

	// 将 request_setudp 写入管道
	send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) + addrsz);

	return 0;
}

const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
	// 这里参考 forward_message_udp 函数, data + n 存储地址信息, data 存储数据内容
	uint8_t * address = (uint8_t *)(msg->data + msg->ud);
	int type = address[0];
	switch(type) {
	case PROTOCOL_UDP:
		*addrsz = 1+2+4;
		break;
	case PROTOCOL_UDPv6:
		*addrsz = 1+2+16;
		break;
	default:
		return NULL;
	}
	return (const struct socket_udp_address *)address;
}
