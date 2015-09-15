--[[
一些科普:

先做如下定义：
登陆服务器 L 。这即是本篇介绍的 LoginServer
登陆点若干 G1, G2, G3 ...
认证平台 A
用户 C


1. C 向 A 发起一次认证请求 (A 通常是第三方认证平台)，获得一个 token 。这个 token 里通常包含有用户名称以及用于校验用户合法性的其它信息。

2. C 将他希望登陆的登陆点 G1 (或其它登陆点，可由系统设计的负载均衡器来选择）以及 step 1 获得的 token 一起发送给 L 。

3. C 和 L 交换后续通讯用的密钥 secret ，并立刻验证。

4. L 校验登陆点是否存在，以及 token 的合法性（此处有可能需要 L 和 A 做一次确认）。

5. （可选步骤）L 检查 C 是否已经登陆，如果已经登陆，向它所在的登陆点（可以是一个，也可以是多个）发送信号，等待登陆点确认。通常这个步骤可以将已登陆的用户登出。

6. L 向 G1 发送用户 C 将登陆的请求，并同时发送 secret 。

7. G1 收到 step 6 的请求后，进行 C 登陆的准备工作（通常是加载数据等），记录 secret ，并由 G1 分配一个 subid 返回给 L。
通常 subid 对于一个 userid 是唯一不重复的。

8. L 将子 id 发送给 C 。子 id 多用于多重登陆（允许同一个账号同时登陆多次），一个 userid 和一个 subid 一起才是一次登陆的 username 。
而每个 username 都对应有唯一的 secret 。

9. C 得到 L 的确认后，断开和 L 的连接。然后连接 G1 ，并利用 username 和 secret 进行握手。

以上流程，任何一个步骤失败，都会中断登陆流程，用户 C 会收到错误码。

登陆点会按照业务的需要，在确认用户登出后，通知 L 。登出可能发生在连接断开（基于长连接的应用）、用户主动退出、一定时间内没有收到用户消息等。

对于同一个用户登陆时，该用户已经在系统中时，通常有三种应对策略：
1. 允许同时登陆。由于每次登陆的 subid 登陆点 不同，所以可以区分同一个账号下的不同实体。

2. 不允许同时登陆，当新的登陆请求到达并验证后，命令上一次登陆的实体登出。登出完成后，接受新的登陆。

3. 如果一个用户在系统中，禁止该用户再次进入。

--]]

local skynet = require "skynet"
require "skynet.manager"
local socket = require "socket"
local crypt = require "crypt"
local table = table
local string = string
local assert = assert

--[[

Protocol:

	line (\n) based text protocol
	登陆服务器和客户端的交互协议基于文本

	1. Server->Client : base64(8bytes random challenge), 这是一个 8 字节长的随机串，用于后序的握手验证。
	2. Client->Server : base64(8bytes handshake client key), 这是一个 8 字节的由客户端发送过来，用于交换 secret 的 key 。
	3. Server: Gen a 8bytes handshake server key, 生成一个用户交换 secret 的 key 。
	4. Server->Client : base64(DH-Exchange(server key)), 利用 DH 密钥交换算法，发送交换过的 server key 。
	5. Server/Client secret := DH-Secret(client key/server key), 服务器和客户端都可以计算出同一个 8 字节的 secret 。
	6. Client->Server : base64(HMAC(challenge, secret)), 回应服务器第一步握手的挑战码，确认握手正常。
	7. Client->Server : DES(secret, base64(token)), 使用 DES 算法，以 secret 做 key 加密传输 token 串。
	8. Server : call auth_handler(token) -> server, uid (A user defined method)
	9. Server : call login_handler(server, uid, secret) ->subid (A user defined method)
	10. Server->Client : 200 base64(subid), 发送确认信息 200 subid ，或发送错误码。

Error Code:
	400 Bad Request . challenge failed, 握手失败
	401 Unauthorized . unauthorized by auth_handler, 自定义的 auth_handler 不认可 token
	403 Forbidden . login_handler failed, 自定义的 login_handler 执行失败
	406 Not Acceptable . already in login (disallow multi login), 该用户已经在登陆中。（只发生在 multilogin 关闭时）

Success:
	200 base64(subid)
]]

-- 只存储 socket 产生的错误
local socket_error = {}

-- 判断 socket 的操作是否正常, v 表示的是 socket 操作后的返回值, 如果为 false or nil, 那么将直接排除错误.
local function assert_socket(service, v, fd)
	if v then
		return v
	else
		skynet.error(string.format("%s failed: socket (fd = %d) closed", service, fd))
		error(socket_error)
	end
end

-- 使用 fd 对应的 socket 发送 text 数据
local function write(service, fd, text)
	assert_socket(service, socket.write(fd, text), fd)
end

-- slave 的逻辑处理
local function launch_slave(auth_handler)

	-- 与 client 的认证逻辑
	local function auth(fd, addr)
		fd = assert(tonumber(fd))
		skynet.error(string.format("connect from %s (fd = %d)", addr, fd))
		socket.start(fd)

		-- set socket buffer limit (8K)
		-- If the attacker send large package, close the socket
		-- 设置 socket buffer 的上限(8K)
		-- 如果攻击者发送大数据包, 那么关闭该 socket
		socket.limit(fd, 8192)

		-- 发送 8 字节长的随机串
		local challenge = crypt.randomkey()
		write("auth", fd, crypt.base64encode(challenge).."\n")

		-- 这是一个 8 字节的由客户端发送过来，用于交换 secret 的 key 。
		local handshake = assert_socket("auth", socket.readline(fd), fd)
		local clientkey = crypt.base64decode(handshake)
		if #clientkey ~= 8 then
			error "Invalid client key"
		end

		-- 生成一个用户交换 secret 的 key 。
		local serverkey = crypt.randomkey()

		-- 利用 DH 密钥交换算法，发送交换过的 server key 。
		write("auth", fd, crypt.base64encode(crypt.dhexchange(serverkey)).."\n")

		-- 服务器和客户端都可以计算出同一个 8 字节的 secret 。
		local secret = crypt.dhsecret(clientkey, serverkey)

		-- 回应服务器第一步握手的挑战码，确认握手正常。
		local response = assert_socket("auth", socket.readline(fd), fd)
		local hmac = crypt.hmac64(challenge, secret)
		if hmac ~= crypt.base64decode(response) then
			write("auth", fd, "400 Bad Request\n")
			error "challenge failed"
		end

		-- 使用 DES 算法，以 secret 做 key 加密传输 token 串。
		local etoken = assert_socket("auth", socket.readline(fd),fd)
		local token = crypt.desdecode(secret, crypt.base64decode(etoken))

		local ok, server, uid = pcall(auth_handler, token)

		socket.abandon(fd)
		return ok, server, uid, secret
	end

	-- 结果返回给 login server 的 master 服务
	local function ret_pack(ok, err, ...)
		if ok then
			skynet.ret(skynet.pack(err, ...))
		else
			if err == socket_error then
				skynet.ret(skynet.pack(nil, "socket error"))
			else
				skynet.ret(skynet.pack(false, err))
			end
		end
	end

	-- 注册 lua 协议, 可以查看下面的 accept 函数, 它发送 fd 和 addr 给 slave
	skynet.dispatch("lua", function(_, _, ...)
		ret_pack(pcall(auth, ...))
	end)
end

-- 在 conf.multilogin 关闭时, 防止用户多次调用 login_handler, 记录已经调用了 login_handler 的用户.
-- 在调用 login_handler 结束后, 删除存储的用户.
local user_login = {}

-- master 在接收到 salve 的 auth_handler 结果之后, 再处理 login_handler
-- @param conf 配置表
-- @param s slave 实例 handle
-- @param fd socket id
-- @param addr 连接的主机地址
local function accept(conf, s, fd, addr)
	-- call slave auth
	-- 让 slave 认证
	local ok, server, uid, secret = skynet.call(s, "lua",  fd, addr)
	socket.start(fd)

	-- 401 Unauthorized. unauthorized by auth_handler, 自定义的 auth_handler 不认可 token
	if not ok then
		if ok ~= nil then
			write("response 401", fd, "401 Unauthorized\n")
		end
		error(server)
	end

	-- 406 Not Acceptable . already in login (disallow multi login), 该用户已经在登陆中。（只发生在 multilogin 关闭时）
	if not conf.multilogin then
		if user_login[uid] then
			write("response 406", fd, "406 Not Acceptable\n")
			error(string.format("User %s is already login", uid))
		end

		user_login[uid] = true
	end

	local ok, err = pcall(conf.login_handler, server, uid, secret)
	-- unlock login
	user_login[uid] = nil

	if ok then
		err = err or ""		-- Success
		write("response 200",fd,  "200 "..crypt.base64encode(err).."\n")
	else	-- 403 Forbidden . login_handler failed, 自定义的 login_handler 执行失败
		write("response 403",fd,  "403 Forbidden\n")
		error(err)
	end
end

-- 启动负责 auth_handler 的 slave, 同时处理 login_handler 和注册 "lua" 协议来处理 command_handler
local function launch_master(conf)
	local instance = conf.instance or 8
	assert(instance > 0)
	local host = conf.host or "0.0.0.0"
	local port = assert(tonumber(conf.port))
	local slave = {}
	local balance = 1

	skynet.dispatch("lua", function(_, source, command, ...)
		skynet.ret(skynet.pack(conf.command_handler(command, ...)))
	end)

	-- 创建其他的 slave 实例
	for i = 1, instance do
		table.insert(slave, skynet.newservice(SERVICE_NAME))
	end

	-- 开始侦听连接
	skynet.error(string.format("login server listen at : %s %d", host, port))
	local id = socket.listen(host, port)
	socket.start(id , function(fd, addr)
		local s = slave[balance]
		balance = balance + 1
		if balance > #slave then
			balance = 1
		end

		-- 使用 slave 验证
		local ok, err = pcall(accept, conf, s, fd, addr)

		if not ok then
			if err ~= socket_error then
				skynet.error(string.format("invalid client (fd = %d) error = %s", fd, err))
			end
			socket.start(fd)
		end
		socket.close(fd)
	end)
end

--[[

local conf = {
	host = "0.0.0.0",	-- 侦听地址
	port = 20001,	-- 端口
	instance = 10, -- 开启的 slave 数量
	multilogin = false, -- 如果你希望用户可以同时登陆，可以打开这个开关
	name = "login", -- 是一个内部使用的名字，不要和 skynet 其它服务重名
	
	------- slave funcs -------
	对一个客户端发送过来的 token （step 2）做验证.
	如果验证不能通过，可以通过 error 抛出异常;
	如果验证通过，需要返回用户希望进入的登陆点（登陆点可以是包含在 token 内由用户自行决定,也可以在这里实现一个负载均衡器来选择）；以及用户名。
	auth_handler

	------- master funcs -------
	你需要实现这个方法，处理当用户已经验证通过后，该如何通知具体的登陆点（server ）。
	框架会交给你用户名（uid）和已经安全交换到的通讯密钥。你需要把它们交给登陆点，并得到确认（等待登陆点准备好后）才可以返回。
	如果关闭了 multilogin ，那么对于同一个 uid ，框架不会同时调用多次 login_handler 。在执行这个函数的过程中，如果用户发起了新的请求，他将直接收到拒绝的返回码。
	如果打开 multilogin ，那么 login_handler 有可能并行执行。由于这个函数在实现时，通常需要调用 skynet.call 让出控制权。所以请小心维护状态。
	login_handler

	command 是第一个参数，通常约定为指令类型。这个函数的返回值会作为回应返回给请求方。
	command_handler(command, ...)
}

--]]

local function login(conf)
	local name = "." .. (conf.name or "login")
	skynet.start(function()
		local loginmaster = skynet.localname(name)
		if loginmaster then
			local auth_handler = assert(conf.auth_handler)
			launch_master = nil
			conf = nil
			launch_slave(auth_handler)
		else
			launch_slave = nil
			conf.auth_handler = nil
			assert(conf.login_handler)
			assert(conf.command_handler)
			skynet.register(name)
			launch_master(conf)
		end
	end)
end

return login
