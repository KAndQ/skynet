--[[ 设计 ]]--

-- snax.msgserver 是一个基于消息请求和回应模式的网关服务器模板。它基于 snax.gateserver 定制，可以接收客户端发起的请求数据包，并给出对应的回应。

-- 和 service/gate.lua 不同，用户在使用它的时候，一个用户的业务处理不基于连接。
-- 即，它不把连接建立作为用户登陆、不在连接断开时让用户登出。用户必须显式的登出系统，或是业务逻辑设计的超时机制导致登出。

-- 和传统的 HTTP 服务不同。在同一个连接上，用户可以发起任意多个请求，并接收多次回应。
-- 请求和回应次序不必保证一致，所以内部用 session 来关联请求和回应。
-- 回应不保证和请求次序相同，也方便在同一个 TCP 连接上发起一些服务器需要很长时间的请求，而不会阻塞后续的快速响应。

-- 在请求回应模式下，服务器无法单向向客户端推送消息。但可以很容易模拟这种业务。你只需要在登入系统后，
-- 立刻发起一个应用层协议要求服务器返回给你最近需要推送给你的消息；如果服务器暂时没有消息推送给客户端，
-- 那么只需要简单的挂起这个 session ，待有消息推送时，服务器通过这个 session 返回即可。

-- 和一般的长连接服务也不同。当客户端和服务器失去连接后，只需要简单的重新接入，就可以继续工作。
-- 在 snax.msgserver 的实现中，我们要求客户端和服务器同时只保证一条有效的通讯连接，
-- 但你可以轻易扩展成多条（但意义不大，因为 session 可以保证慢请求不会阻塞快请求的回应）并发。

local skynet = require "skynet"
local gateserver = require "snax.gateserver"
local netpack = require "netpack"
local crypt = require "crypt"
local socketdriver = require "socketdriver"
local assert = assert
local b64encode = crypt.base64encode
local b64decode = crypt.base64decode

--[[

基础封包协议遵循的 GateServer ，即一个 2 字节的包头加内容，以下说明的是内容的编码。

Protocol:

	All the number type is big-endian
	所有的的数字类型使用 big-endian 方式编码

	Shakehands (The first package)
	握手(第一个数据包)

	Client -> Server :
	第一个握手包的格式, 握手首先由客户端发起

	base64(uid)@base64(server)#base64(subid):index:base64(hmac)

	Server -> Client
	服务器针对握手的确认回应, 回应信息是一行文本

	XXX ErrorCode
		404 User Not Found
		403 Index Expired
		401 Unauthorized
		400 Bad Request
		200 OK

	Req-Resp
	数据的请求回应格式

	Client -> Server : Request, 客户端请求
		word size (Not include self), 2 个字节, 表示数据包大小, 不包括自身
		string content (size-4), 数据内容
		dword session, 4 个字节 session

	Server -> Client : Response, 服务器回应
		word size (Not include self), 2 个字节, 表示数据包大小, 不包括自身
		string content (size-5), 数据内容
		byte ok (1 is ok, 0 is error), 1 个字节, 1 表示正常返回，0 表示异常返回。
		dword session, 4 个字节 session

API:
	把一个登录名(username)转换为 uid, subid, server 三元组
	server.userid(username)
		return uid, subid, server

	把 uid, subid, servername 三元组构造成一个登录名(username)
	server.username(uid, subid, server)
		return username

	你需要在 login_handler 中调用它，注册一个登录名(username)对应的 serect
	server.login(username, secret)
		update user secret

	让一个登录名(username)失效（登出），通常在 logout_handler 里调用。
	server.logout(username)
		user logout

	查询一个登录名(username)对应的连接的 ip 地址，如果没有关联的连接，会返回 nil 。
	server.ip(username)
		return ip when connection establish, or nil

	启动一个 msgserver 。conf 是配置表。配置表和 GateServer 相同，但增加一项 servername ，你需要配置这个登录点的名字。
	server.start(conf)
		start server

Supported skynet command:
	kick username (may used by loginserver)
	login username secret (used by loginserver)
	logout username (used by agent)

Config for server.start:
	conf.expired_number : the number of the response message cached after sending out (default is 128)
	缓存发送出去的响应消息的数量(默认是 128)

	conf.login_handler(uid, secret) -> subid : the function when a new user login, alloc a subid for it. (may call by login server)
	当 1 个新的用户登录的时候运行, 返回 1 个 subid(可能在登录服务器调用)

	conf.logout_handler(uid, subid) : the functon when a user logout. (may call by agent)
	这个函数在用户登出时调用(可能在 agent 调用)

	conf.kick_handler(uid, subid) : the functon when a user logout. (may call by login server)
	这个函数在用户登出时调用(可能在登录服务器调用)

	conf.request_handler(username, session, msg) : the function when recv a new request.
	这个函数在接收 1 个新的请求时调用

	conf.register_handler(servername) : call when gate open
	当网关服务器打开时调用

	conf.disconnect_handler(username) : call when a connection disconnect (afk)
	当 1 个连接断开时调用(afk, 暂离)
]]

-- 模块
local server = {}

-- 注册 PTYPE_CLIENT 协议
skynet.register_protocol {
	name = "client",
	id = skynet.PTYPE_CLIENT,
}

-- username -> user 对象 { secret = secret, version = 0, index = 0, username = base64(uid)@base64(server)#base64(subid), response = {} }
local user_online = {}

-- 
local handshake = {}

-- 建立连接的客户端, socket id -> user 对象
local connection = {}

-- 把一个登录名(username)转换为 uid, subid, server 三元组
-- @param username 以 base64(uid)@base64(server)#base64(subid) 格式组成的字符串
function server.userid(username)
	local uid, servername, subid = username:match "([^@]*)@([^#]*)#(.*)"
	return b64decode(uid), b64decode(subid), b64decode(servername)
end

-- 把 uid, subid, servername 三元组构造成一个登录名(username)
-- @param uid 
-- @param subid
-- @param servername
-- @return 返回 base64(uid)@base64(server)#base64(subid) 格式组成的字符串
function server.username(uid, subid, servername)
	return string.format("%s@%s#%s", b64encode(uid), b64encode(servername), b64encode(tostring(subid)))
end

-- 让一个登录名(username)失效（登出），通常在 logout_handler 里调用。
-- @param username
function server.logout(username)
	local u = user_online[username]
	user_online[username] = nil

	-- 如果当前正在和客户端连接, 那么主动断开连接
	if u.fd then
		gateserver.closeclient(u.fd)
		connection[u.fd] = nil
	end
end

-- 你需要在 login_handler 中调用它，注册一个登录名(username)对应的 serect
-- @param username
-- @param secret
function server.login(username, secret)
	assert(user_online[username] == nil)
	user_online[username] = {
		secret = secret,
		version = 0,
		index = 0,
		username = username,
		response = {},	-- response cache, 响应缓存
	}
end

-- 查询一个登录名(username)对应的连接的 ip 地址，如果没有关联的连接，会返回 nil 。
-- @param username 
-- @return 如果不存在, 那么返回 nil
function server.ip(username)
	local u = user_online[username]
	if u and u.fd then
		return u.ip
	end
end

-- 启动一个 msgserver 。conf 是配置表。配置表和 GateServer 相同，但增加一项 servername ，你需要配置这个登录点的名字。
-- @param conf 启动 msgserver 配置表
function server.start(conf)
	local expired_number = conf.expired_number or 128	-- 缓存发送出去的响应消息的数量(默认是 128)

	local handler = {}

	local CMD = {
		login = assert(conf.login_handler),
		logout = assert(conf.logout_handler),
		kick = assert(conf.kick_handler),
	}

	-- lua 协议消息处理
	-- @param cmd 命令
	-- @param source 服务发送源
	-- @param ... 命令对应函数的执行时的参数
	-- @return 命令函数的返回值
	function handler.command(cmd, source, ...)
		local f = assert(CMD[cmd])
		return f(...)
	end

	-- gateserver 打开侦听 socket
	-- @param source 请求源
	-- @param gateconf 启动 gateserver 配置
	-- @return conf.register_handler 返回值
	function handler.open(source, gateconf)
		local servername = assert(gateconf.servername)
		return conf.register_handler(servername)
	end

	-- 新的连接接入
	-- @param fd 新连接的 socket id
	-- @param addr 新连接的地址
	function handler.connect(fd, addr)
		handshake[fd] = addr
		gateserver.openclient(fd)
	end

	-- 接入的连接断开
	-- @param fd 连接的 fd
	function handler.disconnect(fd)
		handshake[fd] = nil
		local c = connection[fd]
		if c then
			c.fd = nil
			connection[fd] = nil
			if conf.disconnect_handler then
				conf.disconnect_handler(c.username)
			end
		end
	end

	handler.error = handler.disconnect

	-- atomic , no yield
	-- 原子性, 不阻塞

	-- 执行 user 校验的实际逻辑
	-- @param fd 连接的 socket id
	-- @param message 接收到的字符串数据, 以 base64(uid)@base64(server)#base64(subid):index:base64(hmac) 格式组成
	-- @param addr 连接终端的主机地址
	-- @return 认证失败则返回失败信息字符串; 成功返回 nil
	local function do_auth(fd, message, addr)
		-- 解析 message
		local username, index, hmac = string.match(message, "([^:]*):([^:]*):([^:]*)")

		-- 查询 user 对象
		local u = user_online[username]

		-- 还没有该 user
		if u == nil then
			return "404 User Not Found"
		end

		local idx = assert(tonumber(index))
		hmac = b64decode(hmac)

		if idx <= u.version then
			return "403 Index Expired"
		end

		local text = string.format("%s:%s", username, index)
		local v = crypt.hmac_hash(u.secret, text)	-- equivalent to crypt.hmac64(crypt.hashkey(text), u.secret)
		if v ~= hmac then
			return "401 Unauthorized"
		end

		u.version = idx
		u.fd = fd
		u.ip = addr
		connection[fd] = u
	end

	-- user 认证
	-- @param fd 连接的 socket id
	-- @param addr 连接的主机 addr
	-- @param msg 接收到的数据指针
	-- @param sz 指向数据的大小
	local function auth(fd, addr, msg, sz)
		local message = netpack.tostring(msg, sz)	-- 注意, 这里将 msg 指向的内存释放了, msg 的内存复制成 string 对象, 由 lua 管理
		local ok, result = pcall(do_auth, fd, message, addr)
		if not ok then
			skynet.error(result)
			result = "400 Bad Request"
		end

		local close = result ~= nil

		if result == nil then
			result = "200 OK"
		end

		socketdriver.send(fd, netpack.pack(result))

		-- 发生错误关闭连接
		if close then
			gateserver.closeclient(fd)
		end
	end

	local request_handler = assert(conf.request_handler)

	-- u.response is a struct { return_fd, response, version, index }
	-- u.response 是 1 个结构体 { return_fd, response, version, index }
	local function retire_response(u)
		if u.index >= expired_number * 2 then
			local max = 0
			local response = u.response
			for k, p in pairs(response) do
				if p[1] == nil then
					-- request complete, check expired
					if p[4] < expired_number then
						response[k] = nil
					else
						p[4] = p[4] - expired_number
						if p[4] > max then
							max = p[4]
						end
					end
				end
			end
			u.index = max + 1
		end
	end

	local function do_request(fd, message)
		local u = assert(connection[fd], "invalid fd")
		local session = string.unpack(">I4", message, -4)
		message = message:sub(1,-5)
		local p = u.response[session]
		if p then
			-- session can be reuse in the same connection
			-- session 能够在相同的连接重用
			if p[3] == u.version then
				local last = u.response[session]
				u.response[session] = nil
				p = nil
				if last[2] == nil then
					local error_msg = string.format("Conflict session %s", crypt.hexencode(session))
					skynet.error(error_msg)
					error(error_msg)
				end
			end
		end

		if p == nil then
			p = { fd }
			u.response[session] = p
			local ok, result = pcall(conf.request_handler, u.username, message)
			-- NOTICE: YIELD here, socket may close.
			result = result or ""
			if not ok then
				skynet.error(result)
				result = string.pack(">BI4", 0, session)
			else
				result = result .. string.pack(">BI4", 1, session)
			end

			p[2] = string.pack(">s2",result)
			p[3] = u.version
			p[4] = u.index
		else
			-- update version/index, change return fd.
			-- resend response.
			p[1] = fd
			p[3] = u.version
			p[4] = u.index
			if p[2] == nil then
				-- already request, but response is not ready
				return
			end
		end
		u.index = u.index + 1
		-- the return fd is p[1] (fd may change by multi request) check connect
		fd = p[1]
		if connection[fd] then
			socketdriver.send(fd, p[2])
		end
		p[1] = nil
		retire_response(u)
	end

	local function request(fd, msg, sz)
		local message = netpack.tostring(msg, sz)
		local ok, err = pcall(do_request, fd, message)
		-- not atomic, may yield
		-- 非原子性, 可能阻塞
		if not ok then
			skynet.error(string.format("Invalid package %s : %s", err, message))
			if connection[fd] then
				gateserver.closeclient(fd)
			end
		end
	end

	function handler.message(fd, msg, sz)
		local addr = handshake[fd]
		if addr then
			auth(fd,addr,msg,sz)
			handshake[fd] = nil
		else
			request(fd, msg, sz)
		end
	end

	return gateserver.start(handler)
end

return server
