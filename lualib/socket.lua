local driver = require "socketdriver"
local skynet = require "skynet"
local skynet_core = require "skynet.core"
local assert = assert

local socket = {}	-- api
local buffer_pool = {}	-- store all message buffer object, 存储全部消息缓存对象, 这就是 lua-socket.c 里面介绍的 pool, 这个对象只有在当前 lua 沙箱被 close 的时候, 里面的数据(buffer_node)才会被删除.
local socket_pool = setmetatable( -- store all socket object, 存储着所有的 socket 对象, 键是 socket 的 id.
	{},
	{ __gc = function(p)		-- socket_pool 在进行垃圾回收的时候会调用该方法
		-- 关闭掉当前所有的 socket
		for id, v in pairs(p) do
			driver.close(id)
			-- don't need clear v.buffer, because buffer pool will be free at the end
			-- 不必清除 v.buffer, 因为 buffer pool 将在最后被释放掉
			-- 这里的 v 是指 socket
			p[id] = nil
		end
	end
	}
)

local socket_message = {}

-- 唤醒 socket.co
local function wakeup(s)
	local co = s.co
	if co then
		s.co = nil
		skynet.wakeup(co)
	end
end

-- 挂起当前所在的协程
local function suspend(s)
	-- 保证之前没有关联其他的协程
	assert(not s.co)

	s.co = coroutine.running()	-- 得到当前的 coroutine.
	skynet.wait()	-- 把当前的 coroutine 挂起

	-- wakeup closing corouting every time suspend,
	-- because socket.close() will wait last socket buffer operation before clear the buffer.
	-- 每次调用 suspend 函数, 都会唤醒正在 closing 协程, 
	-- 因为在清空 buffer 之前, socket.close() 将等待最后的 socket buffer 操作.
	if s.closing then
		skynet.wakeup(s.closing)
	end
end

-- read skynet_socket.h for these macro
-- SKYNET_SOCKET_TYPE_DATA = 1
-- 从 socket 接收到数据
socket_message[1] = function(id, size, data)
	local s = socket_pool[id]
	if s == nil then
		skynet.error("socket: drop package from " .. id)
		driver.drop(data, size)	-- 释放掉 skynet_socket_message.buffer 的数据
		return
	end

	-- 将读取到的数据压入到 socket_buffer 中, 返回 socket_buffer 当前存储数据的大小
	local sz = driver.push(s.buffer, buffer_pool, data, size)
	local rr = s.read_required
	local rrt = type(rr)
	if rrt == "number" then
		-- read size
		-- 读取大小
		if sz >= rr then	-- 当前存储数据大于需求数据
			s.read_required = nil
			wakeup(s)		-- 唤醒等待读取的协程
		end
	else
		-- 缓存上限报告
		if s.buffer_limit and sz > s.buffer_limit then
			skynet.error(string.format("socket buffer overflow: fd=%d size=%d", id , sz))
			driver.clear(s.buffer, buffer_pool)
			driver.close(id)
			return
		end

		if rrt == "string" then
			-- read line
			-- 检查是否有一行数据可读
			if driver.readline(s.buffer,nil,rr) then
				s.read_required = nil

				-- 唤醒协程
				wakeup(s)
			end
		end
	end
end

-- SKYNET_SOCKET_TYPE_CONNECT = 2
-- socket_server.c:
--		start_socket(socket_server_start, 'S')
--		bind_socket(socket_server_bind, 'B'),
-- 		open_socket(socket_server_connect, 'O'), 如果 open 时是 connecting 状态, 那么 report_connect 成功的话也会返回 SOCKET_OPEN
-- 打开 socket
socket_message[2] = function(id, _ , addr --[[ 这里需要说明下, 存储的不是地址数据 ]])
	local s = socket_pool[id]
	if s == nil then
		return
	end

	-- log remote addr
	s.connected = true

	-- 唤醒协程
	wakeup(s)
end

-- SKYNET_SOCKET_TYPE_CLOSE = 3
-- 关闭 socket
socket_message[3] = function(id)
	local s = socket_pool[id]
	if s == nil then
		return
	end

	s.connected = false

	-- 唤醒协程
	wakeup(s)
end

-- SKYNET_SOCKET_TYPE_ACCEPT = 4
-- 当有新的 socket 接入时
-- id 是正在侦听的 socket id
-- newid 是新连接 socket 的 id
-- addr 新连接的地址信息
socket_message[4] = function(id, newid, addr)
	local s = socket_pool[id]
	if s == nil then
		driver.close(newid)
		return
	end

	s.callback(newid, addr)
end

-- SKYNET_SOCKET_TYPE_ERROR = 5
-- socket 产生错误
socket_message[5] = function(id)
	local s = socket_pool[id]
	if s == nil then
		skynet.error("socket: error on unknown", id)
		return
	end

	if s.connected then
		skynet.error("socket: error on", id)
	end

	s.connected = false

	-- 唤醒协程
	wakeup(s)
end

-- SKYNET_SOCKET_TYPE_UDP = 6
-- 基于 udp 协议接收到信息
socket_message[6] = function(id, size, data, address)
	local s = socket_pool[id]
	if s == nil or s.callback == nil then
		skynet.error("socket: drop udp package from " .. id)
		driver.drop(data, size)
		return
	end

	local str = skynet.tostring(data, size)	-- 将数据压入成 lua 的 string 对象
	skynet_core.trash(data, size)	-- 释放数据资源
	s.callback(str, address)
end

-- 默认警告, 待发送数据达到某个值的时候, 会发出该警告
local function default_warning(id, size)
	local s = socket_pool[id]
	local last = s.warningsize or 0
	if last + 64 < size then	-- if size increase 64K, 如果 size 增长了 64
		s.warningsize = size
		skynet.error(string.format("WARNING: %d K bytes need to send out (fd = %d)", size, id))
	end
	s.warningsize = size
end

-- SKYNET_SOCKET_TYPE_WARNING
-- socket 警告
socket_message[7] = function(id, size)
	local s = socket_pool[id]
	if s then
		local warning = s.warning or default_warning
		warning(id, size)
	end
end

-- 注册 socket 消息类型
skynet.register_protocol {
	name = "socket",
	id = skynet.PTYPE_SOCKET,	-- PTYPE_SOCKET = 6
	unpack = driver.unpack,
	dispatch = function (_, _, t, ...)
		socket_message[t](...)
	end
}

-- 当本机的 socket 与一个远端 socket 连接的时候, 数据的初始化.
local function connect(id, func)
	local newbuffer
	if func == nil then
		newbuffer = driver.buffer()
	end

	local s = {
		id = id,	-- socket id
		buffer = newbuffer,	-- socket_buffer, 作为 listener 的 socket, newbuffer 为 nil.
		connected = false,	-- 是否连接

		-- 有多个含义: 
		-- nil or false: 表示当前没有在进行读取操作
		-- string: 表示行分隔符
		-- number 并且非 0: 表示需要读取的数据大小
		-- number 为 0: 表示有多少就读多少
		-- boolean true: 表示一直读到连接断开为止
		read_required = false,
		
		co = false,			-- 可以理解为正在被阻塞的协程
		callback = func,	-- 基于 tcp 协议, 当前是侦听的 socket, 当有新的 socket 接入时的回调.
		protocol = "TCP",	-- 协议类型
	}

	-- 记录 socket
	socket_pool[id] = s

	-- 阻塞当前协程, 等待被唤醒
	suspend(s)

	if s.connected then
		return id
	else
		socket_pool[id] = nil
	end
end

-- 建立一个 TCP 连接。返回一个数字 id 。
function socket.open(addr, port)
	local id = driver.connect(addr,port)
	return connect(id)
end

-- bind 一个 socket fd
function socket.bind(os_fd)
	local id = driver.bind(os_fd)
	return connect(id)
end

-- 绑定标准输入文件, 这边在输入源操作的时候, 这里也能收到数据.
-- 很屌, 因为底层都操作的都是文件描述符, 所以操作 socket 和操作文件可以看成是一样的.
function socket.stdin()
	return socket.bind(0)
end

-- func 是一个函数。每当一个监听的 id 对应的 socket 上有连接接入的时候，都会调用 func 函数。
-- 这个函数会得到接入连接的 id 以及 ip 地址。你可以做后续操作。
-- 每当 func 函数获得一个新的 socket id 后，并不会立即收到这个 socket 上的数据，也不能向这个 socket 写数据。
-- 这是因为，我们有时会希望把这个 socket 的操作权转让给别的服务去处理。
-- socket 的 id 对于整个 skynet 节点都是公开的。也就是说，你可以把 id 这个数字通过消息发送给其它服务，其他服务也可以去操作它。
-- 任何一个服务只有在调用 socket.start(id) 之后，才可以收到这个 socket 上的数据，或者向这个 socket 写数据。
-- 注：skynet 框架是根据调用 start 这个 api 的位置来决定把对应 socket 上的数据转发到哪里去的。
-- 注意: 调用了 start 之后 socket 才会加入到 event pool 中, 这时才能读取/写入数据, 不然接收到的数据都在 socket 的缓存中.
function socket.start(id, func)
	driver.start(id)
	return connect(id, func)
end

-- 强行关闭一个连接。和 close 不同的是，它不会等待可能存在的其它 coroutine 的读操作。
-- 一般不建议使用这个 API ，但如果你需要在 __gc 元方法中关闭连接的话，shutdown 是一个比 close 更好的选择（因为在 gc 过程中无法切换 coroutine）。
function socket.shutdown(id)
	local s = socket_pool[id]
	if s then

		-- 清空掉 socket_buffer 可读数据
		if s.buffer then
			driver.clear(s.buffer, buffer_pool)
		end

		if s.connected then
			driver.close(id)
		end
	end
end

-- 关闭一个连接，这个 API 有可能阻塞住执行流。
-- 因为如果有其它协程正在阻塞读这个 id 对应的连接，会先驱使读操作结束，close 操作才返回。
function socket.close(id)
	local s = socket_pool[id]

	-- 不存在就不做处理
	if s == nil then
		return
	end

	if s.connected then
		driver.close(s.id)
		
		-- notice: call socket.close in __gc should be carefully,
		-- because skynet.wait never return in __gc, so driver.clear may not be called
		-- 注意: 在 __gc 中调用 socket.close 应该是安全的,
		-- 因为 skynet.wait 不会在 __gc 中返回, 所以 driver.clear 可能不会被调用.
		if s.co then	-- 等待 co 处理完
			-- reading this socket on another coroutine, so don't shutdown (clear the buffer) immediatel
			-- wait reading coroutine read the buffer.
			-- 这个 socket 可能在另外一个协程正在读取, 所以不要立即关闭(清除 buffer),
			-- 等待正在读取的 buffer 读完缓存.
			assert(not s.closing)
			s.closing = coroutine.running()
			skynet.wait()
		else	-- 没有 co, 直接等待 SKYNET_SOCKET_CLOSE 返回
			suspend(s)
		end
		s.connected = false
	end

	socket.shutdown(id)

	-- 保证已经没有协程在阻塞了
	assert(s.lock_set == nil or next(s.lock_set) == nil)
	socket_pool[id] = nil
end

-- 从一个 socket 上读 sz 指定的字节数。如果读到了指定长度的字符串，它把这个字符串返回。
-- 如果连接断开导致字节数不够，将返回一个 false 加上读到的字符串。如果 sz 为 nil ，则返回尽可能多的字节数，但至少读一个字节（若无新数据，会阻塞）。
function socket.read(id, sz)
	local s = socket_pool[id]
	assert(s)

	if sz == nil then
		-- read some bytes
		-- 将当前存储的数据全部读取出来
		local ret = driver.readall(s.buffer, buffer_pool)

		-- 如果读取出数据, 则返回读取的数据
		if ret ~= "" then
			return ret
		end

		-- 如果连接断开导致字节数不够，将返回一个 false 加上读到的字符串。
		if not s.connected then
			return false, ret
		end

		-- 确保没有其他协程在读
		assert(not s.read_required)

		-- 有数据读即可
		s.read_required = 0

		-- 协程阻塞, 等待唤醒
		suspend(s)
		
		-- 将当前存储的数据全部读取出来
		ret = driver.readall(s.buffer, buffer_pool)

		-- 如果读取出数据, 则返回读取的数据
		if ret ~= "" then
			return ret
		else
			return false, ret
		end
	end

	-- 读取 sz 大小的数据
	local ret = driver.pop(s.buffer, buffer_pool, sz)
	if ret then
		return ret
	end

	-- 如果连接断开导致字节数不够，将返回一个 false 加上读到的字符串。
	if not s.connected then
		return false, driver.readall(s.buffer, buffer_pool)
	end

	-- 确保没有其他协程在读
	assert(not s.read_required)

	-- 记录期望读取数据的大小
	s.read_required = sz

	-- 协程阻塞, 等待唤醒
	suspend(s)

	-- 读取 sz 大小的数据, 否则将返回一个 false 加上读到的字符串。
	ret = driver.pop(s.buffer, buffer_pool, sz)
	if ret then
		return ret
	else
		return false, driver.readall(s.buffer, buffer_pool)
	end
end

-- 从一个 socket 上读所有的数据，直到 socket 主动断开，或在其它 coroutine 用 socket.close 关闭它。
function socket.readall(id)
	local s = socket_pool[id]
	assert(s)

	-- 如果 socket 已经关闭了, 那么直接将数据全部读取出来
	if not s.connected then
		local r = driver.readall(s.buffer, buffer_pool)
		return r ~= "" and r
	end

	-- 确保其他协程没有在读
	assert(not s.read_required)

	-- 标记一直读, 直到断开为止
	s.read_required = true

	-- 协程阻塞, 等待唤醒
	suspend(s)

	-- 确保 socket 连接断开
	assert(s.connected == false)

	-- 读取所有的数据
	return driver.readall(s.buffer, buffer_pool)
end

-- 从一个 socket 上读一行数据。sep 指行分割符。默认的 sep 为 "\n"。读到的字符串是不包含这个分割符的。
function socket.readline(id, sep)
	sep = sep or "\n"
	local s = socket_pool[id]
	assert(s)

	-- 读取一行数据, 如果有可读的数据, 则直接返回
	local ret = driver.readline(s.buffer, buffer_pool, sep)
	if ret then
		return ret
	end

	-- 如果连接断开导致字节数不够，将返回一个 false 加上读到的字符串。
	if not s.connected then
		return false, driver.readall(s.buffer, buffer_pool)
	end

	-- 确保没有其他协程正在读
	assert(not s.read_required)

	-- 记录分隔符
	s.read_required = sep

	-- 协程阻塞, 等待唤醒
	suspend(s)

	-- 如果处于连接状态, 读取一行数据, 否则将返回一个 false 加上读到的字符串。
	if s.connected then
		return driver.readline(s.buffer, buffer_pool, sep)
	else
		return false, driver.readall(s.buffer, buffer_pool)
	end
end

-- 等待一个 socket 可读。
function socket.block(id)
	local s = socket_pool[id]
	if not s or not s.connected then
		return false
	end

	-- 确保没有其他协程在读
	assert(not s.read_required)

	-- 有数据读即可
	s.read_required = 0

	-- 阻塞协程, 有数据可读的时候唤醒
	suspend(s)

	return s.connected
end

socket.write = assert(driver.send)
socket.lwrite = assert(driver.lsend)
socket.header = assert(driver.header)

-- 判断 id 对应的 socket 是否无效, 无效返回 true, 否则返回 false
function socket.invalid(id)
	return socket_pool[id] == nil
end

-- 监听一个端口，返回一个 id ，供 start 使用。
function socket.listen(host, port, backlog)
	if port == nil then
		host, port = string.match(host, "([^:]+):(.+)$")
		port = tonumber(port)
	end
	return driver.listen(host, port, backlog)
end

-- 挂起 id 对应的 socket 所在的 coroutine.
function socket.lock(id)
	local s = socket_pool[id]
	assert(s)

	-- 创建 lock
	local lock_set = s.lock
	if not lock_set then
		lock_set = {}
		s.lock = lock_set
	end

	-- 第一个非协程
	if #lock_set == 0 then
		lock_set[1] = true
	else	-- 之后的元素才插入协程
		local co = coroutine.running()
		table.insert(lock_set, co)
		skynet.wait()
	end
end

-- 唤醒 id 之前挂起的 coroutine.
-- @param id socket id
-- @return nil
function socket.unlock(id)
	local s = socket_pool[id]
	assert(s)

	-- 确认存在 lock
	local lock_set = assert(s.lock)

	-- 移除第一个元素, 非协程
	table.remove(lock_set,1)

	-- 取出真正的协程元素, 并且唤醒
	local co = lock_set[1]
	if co then
		skynet.wakeup(co)
	end
end

-- abandon use to forward socket id to other service
-- you must call socket.start(id) later in other service
-- 清除 socket id 在本服务内的数据结构，但并不关闭这个 socket 。这可以用于你把 id 发送给其它服务，以转交 socket 的控制权。
-- 你必须在其他的服务调用 socket.start(id) 函数.
function socket.abandon(id)
	local s = socket_pool[id]
	if s and s.buffer then
		driver.clear(s.buffer,buffer_pool)	-- 删除 socket_buffer 里面的数据
	end
	socket_pool[id] = nil
end

-- 设置 socket_buffer 的缓存上限, 当接收到的缓存数据大于这个值的时候, 将报告错误, 并关闭掉这个 socket.
-- 默认为 nil, 没有限制
function socket.limit(id, limit)
	local s = assert(socket_pool[id])
	s.buffer_limit = limit
end

---------------------- UDP

local udp_socket = {}	-- 暂时没有使用

-- 创建 udp 对象
local function create_udp_object(id, cb)
	socket_pool[id] = {
		id = id,	-- socket id
		connected = true,	-- 连接标志
		protocol = "UDP",	-- 协议类型
		callback = cb,	-- 基于 udp 协议的 socket, 当接收到数据时的回调
	}
end

-- 创建一个 udp handle ，并给它绑定一个 callback 函数。当这个 handle 收到 udp 消息时，callback 函数将被触发。
-- 第一个参数是一个 callback 函数，它会收到两个参数。str 是一个字符串即收到的包内容，from 是一个表示消息来源的字符串用于返回这条消息（见 socket.sendto）。
-- 第二个参数是一个字符串表示绑定的 ip 地址。如果你不写，默认为 ipv4 的 0.0.0.0 。
-- 第三个参数是一个数字， 表示绑定的端口。如果不写或传 0 ，这表示仅创建一个 udp handle （用于发送），但不绑定固定端口。
function socket.udp(callback, host, port)
	local id = driver.udp(host, port)
	create_udp_object(id, callback)
	return id
end

-- 你可以给一个 udp handle 设置一个默认的发送目的地址。
-- 当你用 socket.udp 创建出一个非监听状态的 handle 时，设置目的地址非常有用。
-- 因为你很难有别的方法获得一个有效的供 socket.sendto 使用的地址串。
-- 这里 callback 是可选项，通常你应该在 socket.udp 创建出 handle 时就设置好 callback 函数。
-- 但有时，handle 并不是当前 service 创建而是由别处创建出来的。
-- 这种情况，你可以用 socket.start 重设 handle 的所有权，并用这个函数设置 callback 函数。
function socket.udp_connect(id, addr, port, callback)
	local obj = socket_pool[id]
	if obj then
		assert(obj.protocol == "UDP")
		if callback then
			obj.callback = callback
		end
	else
		create_udp_object(id, callback)
	end
	driver.udp_connect(id, addr, port)
end

socket.sendto = assert(driver.udp_send)
socket.udp_address = assert(driver.udp_address)

-- 当 id 对应的 socket 上待发的数据超过 1M 字节后，系统将回调 callback 以示警告。
-- function callback(id, size) 回调函数接收两个参数 id 和 size ，size 的单位是 K 。
-- 如果你不设回调，那么将每增加 64K 利用 skynet.error 写一行错误信息。
function socket.warning(id, callback)
	local obj = socket_pool[id]
	assert(obj)
	obj.warning = callback
end

return socket
