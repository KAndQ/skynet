-- 这个模块不可以和 Socket 库一起使用。因为这个模板接管了 socket 类的消息。
-- 网管服务器的实现, 客户端与连接这个服务连接, 这个服务接收到数据之后, 伪造 client 给 agent 发送接收到的数据, 由 agent 处理实际的逻辑.
-- 这个服务实际上只是做数据的转发与 socket 的连接管理.

local skynet = require "skynet"
local netpack = require "netpack"
local socketdriver = require "socketdriver"

-- 模块表
local gateserver = {}

local socket	-- listen socket, 侦听的 socket
local queue		-- message queue, 消息队列, userdata 类型
local maxclient	-- max client, 最大连接的客户端数量
local client_number = 0		-- 当前连接的客户端数量
local CMD = setmetatable({}, { __gc = function() netpack.clear(queue) end })	-- 命令处理函数集合, 接收 lua 类型的消息
local nodelay = false	-- 设置是否启用 Nagle 算法

-- 记录与当前 gateserver 连接的 socket id, 键是 socket id, 值是布尔值(true/false)
local connection = {}

-- 你需要调用 openclient 让 fd 上的消息进入。
-- 默认状态下， fd 仅仅是连接上你的服务器，但无法发送消息给你。
-- 这个步骤需要你显式的调用是因为，或许你需要在新连接建立后，把 fd 的控制权转交给别的服务。
-- 那么你可以在一切准备好以后，再放行消息。
function gateserver.openclient(fd)
	if connection[fd] then
		socketdriver.start(fd)
	end
end

-- 主动踢掉一个连接。
function gateserver.closeclient(fd)
	local c = connection[fd]
	if c then
		connection[fd] = false
		socketdriver.close(fd)
	end
end

--[[

handler 是一组自定义的消息处理函数, 分别有: 

当一个新连接建立后，connect 方法被调用。传入连接的 socket fd 和新连接的 ip 地址（通常用于 log 输出）。
handler.connect(fd, ipaddr)

当一个连接断开，disconnect 被调用，fd 表示是哪个连接。
handler.disconnect(fd)

当一个连接异常（通常意味着断开），error 被调用，除了 fd ，还会拿到错误信息 msg（通常用于 log 输出）。
handler.error(fd, msg)

如果你希望让服务处理一些 skynet 内部消息，可以注册 command 方法。收到 lua 协议的 skynet 消息，会调用这个方法。
cmd 是消息的第一个值，通常约定为一个字符串，指明是什么指令。source 是消息的来源地址。
这个方法的返回值，会通过 skynet.ret/skynet.pack 返回给来源服务。
handler.command(cmd, source, ...)

监听端口打开的时候，做一些初始化操作，可以提供 open 这个方法。source 是请求来源地址，conf 是开启 gate 服务的参数表。
handler.open(source, conf)

当一个完整的包被切分好后，message 方法被调用。这里 msg 是一个 C 指针、sz 是一个数字，表示包的长度（C 指针指向的内存块的长度）。
注意：这个 C 指针需要在处理完毕后调用 C 方法 skynet_free 释放。（通常建议直接用封装好的库 netpack.tostring 来做这些底层的数据处理）；
或是通过 skynet.redirect 转发给别的 skynet 服务处理。
handler.message(fd, msg, sz)

当 fd 上待发送的数据累积超过 1M 字节后，将回调这个方法。你也可以忽略这个消息。
handler.warning(fd, size)

--]]

-- 启动一个网关服务
-- @param handler table 类型, 如上定义
function gateserver.start(handler)
	
	-- 必须存在的函数
	assert(handler.message)
	assert(handler.connect)

	-- 开启侦听 socket
	function CMD.open(source, conf)
		assert(not socket)
		local address = conf.address or "0.0.0.0"
		local port = assert(conf.port)
		maxclient = conf.maxclient or 1024
		nodelay = conf.nodelay
		skynet.error(string.format("Listen on %s:%d", address, port))
		socket = socketdriver.listen(address, port)
		socketdriver.start(socket)
		if handler.open then
			return handler.open(source, conf)
		end
	end

	-- 关闭侦听 socket
	function CMD.close()
		assert(socket)
		socketdriver.close(socket)
		socket = nil
	end

	-- 用于 socket 协议处理函数的集合
	local MSG = {}

	-- 当从 socket 接收完整数据时的处理, handler 处理从 socket 接收到的内容, 对应 TYPE_DATA
	local function dispatch_msg(fd, msg, sz)
		if connection[fd] then
			handler.message(fd, msg, sz)
		else
			skynet.error(string.format("Drop message from fd (%d) : %s", fd, netpack.tostring(msg, sz)))
		end
	end

	MSG.data = dispatch_msg

	-- 当从 socket 接收到数据时的处理, 从 queue 中弹出数据处理, 对应 TYPE_MORE
	local function dispatch_queue()
		local fd, msg, sz = netpack.pop(queue)
		if fd then
			-- may dispatch even the handler.message blocked
			-- If the handler.message never block, the queue should be empty, so only fork once and then exit.
			-- handler.message 可能会引发阻塞
			-- 如果 handler.message 从不引发阻塞, queue 应该为空, 所以只需要 fork 一次然后退出
			skynet.fork(dispatch_queue)		-- 其他的协程会再处理 pop 出来的数据
			dispatch_msg(fd, msg, sz)

			for fd, msg, sz in netpack.pop, queue do
				dispatch_msg(fd, msg, sz)
			end
		end
	end

	MSG.more = dispatch_queue

	-- 当有新的 socket 连接到 gateserver 的时候触发, 对应 TYPE_OPEN
	function MSG.open(fd, msg)
		-- 确保没有超过最大连接数量, 如果超过, 直接关闭连接
		if client_number >= maxclient then
			socketdriver.close(fd)
			return
		end

		-- 设置是否启用 Nagle 算法
		if nodelay then
			socketdriver.nodelay(fd)
		end

		connection[fd] = true
		client_number = client_number + 1
		handler.connect(fd, msg)
	end

	-- 关闭 1 个连接
	local function close_fd(fd)
		local c = connection[fd]
		if c ~= nil then
			connection[fd] = nil
			client_number = client_number - 1
		end
	end

	-- 对应 TYPE_CLOSE
	function MSG.close(fd)
		if fd ~= socket then
			if handler.disconnect then
				handler.disconnect(fd)
			end
			close_fd(fd)
		end
	end

	-- 对应 TYPE_ERROR
	function MSG.error(fd, msg)
		if fd == socket then
			socketdriver.close(fd)
			skynet.error(msg)
		else
			if handler.error then
				handler.error(fd, msg)
			end
			close_fd(fd)
		end
	end

	-- 对应 TYPE_WARNING
	function MSG.warning(fd, size)
		if handler.warning then
			handler.warning(fd, size)
		end
	end

	-- 注册 socket 类型的协议, 所以不能和 socket.lua 模块同时使用
	skynet.register_protocol {
		name = "socket",
		id = skynet.PTYPE_SOCKET,	-- PTYPE_SOCKET = 6
		unpack = function (msg, sz)
			return netpack.filter(queue, msg, sz)
		end,

		dispatch = function (_, _, q, type, ...)
			queue = q
			if type then
				MSG[type](...)
			end
		end
	}

	-- 注册 lua 协议处理函数, 在 cmd 等于 open 或 close 时, 会调用 CMD 内的函数;
	-- 否则使用 handler.command 函数, 并且将 cmd 和 address(source) 当作参数传入;
	skynet.start(function()
		skynet.dispatch("lua", function (_, address, cmd, ...)
			local f = CMD[cmd]
			if f then
				skynet.ret(skynet.pack(f(address, ...)))
			else
				skynet.ret(skynet.pack(handler.command(cmd, address, ...)))
			end
		end)
	end)
end

return gateserver
