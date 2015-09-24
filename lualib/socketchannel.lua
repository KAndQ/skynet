--[[
实现 socketchannel 的需求:

请求回应模式是和外部服务交互时所用到的最常用模式之一。通常的协议设计方式有两种。

1. 每个请求包对应一个回应包，由 TCP 协议保证时序。redis 的协议就是一个典型。
每个 redis 请求都必须有一个回应，但不必收到回应才可以发送下一个请求。

2. 发起每个请求时带一个唯一 session 标识，在发送回应时，带上这个标识。这样设计可以不要求每个请求都一定要有回应，
且不必遵循先提出的请求先回应的时序。MongoDB 的通讯协议就是这样设计的。

对于第一种模式，用 skynet 的 Socket API 很容易实现，但如果在一个 coroutine 中读写一个 socket 的话，
由于读的过程是阻塞的，这会导致吞吐量下降（前一个回应没有收到时，无法发送下一个请求）。

对于第二种模式，需要用 skynet.fork 开启一个新线程来收取回应包，并自行和请求对应起来，实现比较繁琐。

--]]

local skynet = require "skynet"
local socket = require "socket"
local socketdriver = require "socketdriver"

-- channel support auto reconnect , and capture socket error in request/response transaction
-- channel 支持自动重连, 并且在处理 request/response 事物的时候捕获 socket 错误.
-- { host = "", port = , auth = function(so) , response = function(so) session, data }

local socket_channel = {}	-- 模块表

-- 用于创建 channel 对象
local channel = {}
local channel_meta = { __index = channel }

-- 用于创建一个关联 socket id 的对象, channel.__sock 对象的原表
local channel_socket = {}
local channel_socket_meta = {
	__index = channel_socket,
	__gc = function(cs)
		local fd = cs[1]
		cs[1] = false
		if fd then
			socket.shutdown(fd)
		end
	end
}

local socket_error = setmetatable({}, {__tostring = function() return "[Error: socket]" end })	-- alias for error object, 用于 socket 产生的错误
socket_channel.error = socket_error

-- 创建 channel 对象, desc 是 table 类型, 具体介绍查看: https://github.com/cloudwu/skynet/wiki/SocketChannel
-- 对于协议模式一: desc 需要包含 host 和 port 字段;
-- 对于协议模式二: desc 需要包含 host, port, response 字段, response 对应的是函数类型, 作用是解析回应包;
function socket_channel.channel(desc)
	local c = {
		__host = assert(desc.host),	-- ip 地址或者域名
		__port = assert(desc.port),	-- port 端口号
		__backup = desc.backup,		-- 备用的连接主机, table 类型: 存储的值如果是 table 类型, 那么格式是 { host = "", port = 1234 }; 如果值是字符串类型, 那么表示的 host, 连接时使用 __port.

		-- 由于连接可能发生在任何 request 之前（只要前一次操作检测到连接是断开状态就会重新发起连接），
		-- 所以 socket channel 支持认证流程，允许在建立连接后，立刻做一些交互。如果开启这个功能，
		-- 需要在创建 channel 时，填写一个 auth 函数。和 response 函数一样，会给它传入一个 sock 对象。
		-- auth 函数不需要返回值，如果认证失败，在 auth 函数中抛出 error 即可。
		__auth = desc.auth,			-- 认证函数
		__authcoroutine = false,	-- 认证协程

		__response = desc.response,	-- It's for session mode, 用于 session 模式(协议模式二)
		__request = {},	-- request seq { response func or session }	-- It's for order mode, 请求序列 { 响应函数或者 session }, 用于顺序模式(协议模式一)
		__thread = {}, -- coroutine seq or session->coroutine map, 协程序列或者 session->coroutine 的 map 表
		__result = {}, -- response result { coroutine -> result }, 响应结果(true/false, 如果是 table 类型时, 说明该 socket 已经 close 了), { coroutine -> result } 表
		__result_data = {},	-- 响应的数据
		__connecting = {},	-- 记录其他调用 connect 的协程
		__sock = false,		-- 以 channel_socket_meta 作为原表的对象, [1] 元素是 socket id
		__closed = false,	-- 判断 channel 是否 close, 调用 close 方法
		__nodelay = desc.nodelay,	-- 设置 socket 的 nodelay 选项
	}

	return setmetatable(c, channel_meta)
end

-- 关闭 socket, 传入参数 channel 对象
local function close_channel_socket(self)
	if self.__sock then
		local so = self.__sock
		self.__sock = false
		-- never raise error
		-- 绝不会抛出错误
		pcall(socket.close, so[1])
	end
end

-- 唤醒所有的协程, 并且清空所有记录的协程对象, 同时赋予错误信息.
local function wakeup_all(self, errmsg)
	if self.__response then	-- 协议模式二

		-- __thread 是 session->coroutine 表, 清除 __thread
		for k, co in pairs(self.__thread) do
			self.__thread[k] = nil
			self.__result[co] = socket_error
			self.__result_data[co] = errmsg
			skynet.wakeup(co)
		end
	else	-- 协议模式一
		-- 清除请求
		for i = 1, #self.__request do
			self.__request[i] = nil
		end

		-- __thread 是序列, 清除 __thread
		for i = 1, #self.__thread do
			local co = self.__thread[i]
			self.__thread[i] = nil
			self.__result[co] = socket_error
			self.__result_data[co] = errmsg
			skynet.wakeup(co)
		end
	end
end

-- 基于协议模式二, 通过 session 调度协程
local function dispatch_by_session(self)
	local response = self.__response
	-- response() return session
	-- response() 函数返回 session
	while self.__sock do
		local ok, session, result_ok, result_data, padding = pcall(response, self.__sock)	-- 注意, 这里有可能会阻塞
		--[[
		返回值介绍: 
		ok: 函数调用是否发生 error;
		session: 回应包的 session;
		result_ok: 包是否解析正确（同模式 1 ）;
		result_data: 回应内容;
		padding: 表明了后续是否还有该长消息的后续部分。
		--]]

		if ok and session then
			local co = self.__thread[session]
			if co then
				if padding and result_ok then
					-- If padding is true, append result_data to a table (self.__result_data[co])
					-- 如果 padding 为 true, 追加 result_data 到 self.__result_data[co]
					local result = self.__result_data[co] or {}
					self.__result_data[co] = result
					table.insert(result, result_data)
				else
					self.__thread[session] = nil
					self.__result[co] = result_ok
					if result_ok and self.__result_data[co] then
						table.insert(self.__result_data[co], result_data)
					else
						self.__result_data[co] = result_data
					end
					skynet.wakeup(co)
				end
			else
				self.__thread[session] = nil
				skynet.error("socket: unknown session :", session)
			end
		else
			close_channel_socket(self)

			local errormsg
			if session ~= socket_error then
				errormsg = session
			end

			wakeup_all(self, errormsg)
		end
	end
end

-- 基于协议模式一, 弹出 result 和协程
local function pop_response(self)
	return table.remove(self.__request, 1), table.remove(self.__thread, 1)
end

-- 压入响应.
-- 协议模式一: response 是 1 个函数;
-- 协议模式二: response 是 session.
local function push_response(self, response, co)
	if self.__response then
		-- response is session
		-- response 参数是 session
		self.__thread[response] = co
	else
		-- response is a function, push it to __request
		-- response 是 1 个函数, 将它压入到 __request 中
		table.insert(self.__request, response)
		table.insert(self.__thread, co)
	end
end

-- 基于模式一: 通过顺序来调度协程
local function dispatch_by_order(self)
	while self.__sock do
		local func, co = pop_response(self)
		if func == nil then
			if not socket.block(self.__sock[1]) then	-- 确认当前的 socket 是否有效, 如果无效就 close socket
				close_channel_socket(self)
				wakeup_all(self)
			end
		else
			local ok, result_ok, result_data, padding = pcall(func, self.__sock)	-- 注意, 这里有可能会阻塞
			--[[
			返回值介绍: 
			ok: 函数调用是否发生 error;
			result_ok: 包是否解析正确;
			result_data: 回应内容;
			padding: 表明了后续是否还有该长消息的后续部分。
			--]]

			if ok then
				if padding and result_ok then
					-- if padding is true, wait for next result_data
					-- self.__result_data[co] is a table
					-- 如果 padding 为 true, 等待下一个 result_data
					-- self.__result_data[co] 是一个 table
					local result = self.__result_data[co] or {}
					self.__result_data[co] = result
					table.insert(result, result_data)
				else
					self.__result[co] = result_ok
					if result_ok and self.__result_data[co] then
						table.insert(self.__result_data[co], result_data)
					else
						self.__result_data[co] = result_data
					end
					skynet.wakeup(co)
				end
			else
				close_channel_socket(self)

				local errmsg
				if result_ok ~= socket_error then
					errmsg = result_ok
				end

				-- 因为这个 co 已经不在序列中了, 所以需要单独处理
				self.__result[co] = socket_error
				self.__result_data[co] = errmsg
				skynet.wakeup(co)

				wakeup_all(self, errmsg)
			end
		end
	end
end

-- 调度协程, 整合了 dispatch_by_session 和 dispatch_by_order
-- 根据当前的模式决定使用的调度方式.
local function dispatch_function(self)
	if self.__response then
		return dispatch_by_session
	else
		return dispatch_by_order
	end
end

-- 使用 __backup 配置, 连接 __backup 里面的主机.
-- 一旦连接成功, 返回连接的 socket id
local function connect_backup(self)
	if self.__backup then
		for _, addr in ipairs(self.__backup) do
			local host, port
			if type(addr) == "table" then
				host, port = addr.host, addr.port
			else
				host = addr
				port = self.__port
			end

			skynet.error("socket: connect to backup host", host, port)

			local fd = socket.open(host, port)
			if fd then
				self.__host = host
				self.__port = port
				return fd
			end
		end
	end
end

-- 连接主机, 成功连接返回 true
local function connect_once(self)
	if self.__closed then
		return false
	end

	-- 确认没有建立 socket 连接, 同时没有开启认证协程
	assert(not self.__sock and not self.__authcoroutine)

	-- 连接主机, 如果 __host 和 __port 的连接无效, 使用 __backup 的配置连接,
	-- 如果都连接无效就返回.
	local fd = socket.open(self.__host, self.__port)
	if not fd then
		fd = connect_backup(self)
		if not fd then
			return false
		end
	end

	if self.__nodelay then
		socketdriver.nodelay(fd)
	end

	-- 创建 channel_socket
	self.__sock = setmetatable( {fd} , channel_socket_meta )

	-- 开启新的协程, 用于调用结果
	skynet.fork(dispatch_function(self), self)

	-- 认证
	if self.__auth then
		self.__authcoroutine = coroutine.running()
		local ok , message = pcall(self.__auth, self)
		if not ok then
			close_channel_socket(self)
			if message ~= socket_error then
				self.__authcoroutine = false
				skynet.error("socket: auth failed", message)
			end
		end

		self.__authcoroutine = false

		if ok and not self.__sock then
			-- auth may change host, so connect again
			-- 认证可能改变主机, 所以重新连接 1 次
			return connect_once(self)
		end

		return ok
	end

	return true
end

-- 尝试连接到主机, 成功连接返回 true. once: true/false, 表示是否只连接 1 次, 如果为 false, 那么会无限的尝试连接.
local function try_connect(self, once)
	local t = 0
	while not self.__closed do
		if connect_once(self) then
			if not once then
				skynet.error("socket: connect to", self.__host, self.__port)
			end
			return true
		elseif once then
			return false
		end

		-- 在连接不成功的情况下, 延长挂起时间, 最大不超过 11s(每 11s 打印一次信息).
		if t > 1000 then
			skynet.error("socket: try to reconnect", self.__host, self.__port)
			skynet.sleep(t)
			t = 0
		else
			skynet.sleep(t)
		end

		t = t + 100
	end
end

-- 检查连接是否有效, true 表示已经连接; false 表示已经 close; nil 表示还未连接.
local function check_connection(self)
	if self.__sock then
		local authco = self.__authcoroutine
		if not authco then
			return true
		end

		if authco == coroutine.running() then
			-- authing
			-- 正在认证
			return true
		end
	end

	if self.__closed then
		return false
	end
end

-- 阻塞连接, 保证只有 1 个协程正在连接, 连接成功返回 true.
local function block_connect(self, once)
	local r = check_connection(self)
	if r ~= nil then
		return r
	end

	if #self.__connecting > 0 then
		-- connecting in other coroutine
		-- 尝试连接的其他协程, 只是记录协程, 但是不执行连接操作
		local co = coroutine.running()
		table.insert(self.__connecting, co)
		skynet.wait()
	else
		-- 实际进行连接的协程
		self.__connecting[1] = true
		try_connect(self, once)
		self.__connecting[1] = nil

		-- 唤醒其他的 connect 协程
		for i=2, #self.__connecting do
			local co = self.__connecting[i]
			self.__connecting[i] = nil
			skynet.wakeup(co)
		end
	end

	r = check_connection(self)
	if r == nil then
		error(string.format("Connect to %s:%d failed", self.__host, self.__port))
	else
		return r
	end
end

-- 尝试连接一次。如果失败，抛出 error 。这里参数 true 表示只尝试一次，如果不填这个参数，则一直重试下去。
function channel:connect(once)
	if self.__closed then
		self.__closed = false
	end

	return block_connect(self, once)
end

-- 等待响应.
-- 协议模式一: response 是函数;
-- 协议模式二: response 是 session.
local function wait_for_response(self, response)
	local co = coroutine.running()
	push_response(self, response, co)
	
	-- 阻塞, 等待响应时唤醒
	skynet.wait()

	-- 拿到结果
	local result = self.__result[co]
	self.__result[co] = nil

	-- 拿到结果数据
	local result_data = self.__result_data[co]
	self.__result_data[co] = nil

	if result == socket_error then		-- 这是 socket 方面发生的错误, wakeup_all 会设置 result 为 socket_error
		if result_data then
			error(result_data)
		else
			error(socket_error)
		end
	else
		assert(result, result_data)		-- 这是 response 可能发生的错误
		return result_data
	end
end

local socket_write = socket.write
local socket_lwrite = socket.lwrite

--[[ 
给连接的主机发送请求.
也可用于仅发包而不接收回应。只需要在 request 调用时不填写 response 即可。

在模式 1 下:
	request 是一个字符串，即请求包。
	response 是一个 function ，用来收取回应包。
	padding ，这是用来将体积巨大的消息拆分成多个包发出用的, padding 需求是一个 table ，里面有若干字符串。

在模式 2 下:
	第 2 个参数不再是 response 函数, 而是一个 session 。这个 session 可以是任意类型，
	但需要和 response 函数返回的类型一致。socket channel 会帮你匹配 session 而让 request 返回正确的值。
--]]
function channel:request(request, response, padding)
	assert(block_connect(self, true))	-- connect once
	local fd = self.__sock[1]

	if padding then
		-- padding may be a table, to support multi part request
		-- multi part request use low priority socket write
		-- socket_lwrite returns nothing
		-- padding 参数可能是 1 个table, 支持多个请求,
		-- 多个请求使用低优先级的写队列.
		-- socket_lwrite 函数什么也不返回
		socket_lwrite(fd , request)
		for _,v in ipairs(padding) do
			socket_lwrite(fd, v)
		end
	else
		if not socket_write(fd, request) then
			close_channel_socket(self)
			wakeup_all(self)
			error(socket_error)
		end
	end

	if response == nil then
		-- no response
		-- 没有回应处理
		return
	end

	-- 等待回应, 并且返回
	return wait_for_response(self, response)
end

-- 用来单向接收一个包。
function channel:response(response)
	assert(block_connect(self))

	return wait_for_response(self, response)
end

-- 可以关闭一个 channel ，通常你可以不必主动关闭它，gc 会回收 channel 占用的资源。
function channel:close()
	if not self.__closed then
		self.__closed = true
		close_channel_socket(self)
	end
end

-- 设置 channel 的 host 和 port, 如果 channel 是连接状态, 那么将关闭之前的连接
function channel:changehost(host, port)
	self.__host = host
	if port then
		self.__port = port
	end
	if not self.__closed then
		close_channel_socket(self)
	end
end

-- 设置 __backup 选项
function channel:changebackup(backup)
	self.__backup = backup
end

channel_meta.__gc = channel.close

-- 给 socket 的函数做一次包装.
-- 作用: 首先方便使用语法糖 obj:func() 这种语法; 接着在内部实现中使用 obj[1] 元素; 最后对返回值做检查; 其实 obj 就是 channel.__sock
local function wrapper_socket_function(f)
	return function(self, ...)
		local result = f(self[1], ...)
		if not result then
			error(socket_error)
		else
			return result
		end
	end
end

-- 包装 socket.read
channel_socket.read = wrapper_socket_function(socket.read)

-- 包装 socket.readline
channel_socket.readline = wrapper_socket_function(socket.readline)

return socket_channel
