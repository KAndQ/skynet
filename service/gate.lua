-- gate.lua 是一个完整的网管服务器, 从 gateserver 接收到消息之后, 转发数据给 agent 服务.
-- 对于发送给 watchdog 的 "socket" 类型消息是不需要返回的, 详细可以查看 watchdog.lua.

local skynet = require "skynet"
local gateserver = require "snax.gateserver"
local netpack = require "netpack"

local watchdog	-- watchdog 服务 handler
local connection = {}	-- fd -> connection : { fd, client, agent, ip, mode }, socket id 对 connection 对象的映射
local forwarding = {}	-- agent -> connection, agent 服务 handler 对 connection 的映射

-- 注册 PTYPE_CLIENT 协议, 转发的时候, 会以这个类型转发给 agent 服务
skynet.register_protocol {
	name = "client",
	id = skynet.PTYPE_CLIENT,
}

local handler = {}

-- 侦听打开的时运行
-- @param source 请求开启 gateserver 的服务源
-- @param conf 打开 gateserver 时的配置
function handler.open(source, conf)
	-- 设置 watchdog handler
	watchdog = conf.watchdog or source
end

-- 每次接收到消息时运行
-- @param fd socket id
-- @param msg 接收到的数据内容
-- @param sz 接收到的数据内容大小
function handler.message(fd, msg, sz)
	-- recv a package, forward it
	-- 接收 1 个数据包, 转发数据包到 agent 服务
	local c = connection[fd]
	local agent = c.agent
	if agent then
		-- 将 msg 内存数据交给 skynet 框架管理
		skynet.redirect(agent, c.client, "client", 0, msg, sz)
	else
		-- 注意, 这里通过 netpack.tostring 方法释放掉 msg 内存数据
		skynet.send(watchdog, "lua", "socket", "data", fd, netpack.tostring(msg, sz))
	end
end

-- 当新连接建立后运行
-- @param fd socket id
-- @param addr 连接端的地址
function handler.connect(fd, addr)
	local c = {
		fd = fd,	-- socket id
		ip = addr,	-- 远程主机的地址
	}
	connection[fd] = c
	skynet.send(watchdog, "lua", "socket", "open", fd, addr)
end

-- 设置 connection 对象不转发消息
-- @param c connection 对象
local function unforward(c)
	if c.agent then
		forwarding[c.agent] = nil
		c.agent = nil
		c.client = nil
	end
end

-- 删除 connection 对象
-- @param fd socket id
local function close_fd(fd)
	local c = connection[fd]
	if c then
		unforward(c)
		connection[fd] = nil
	end
end

-- 连接断开时调用
-- @param fd socket id
function handler.disconnect(fd)
	close_fd(fd)
	skynet.send(watchdog, "lua", "socket", "close", fd)
end

-- 连接发生错误时调用
-- @param fd socket id
-- @param msg 错误信息字符串
function handler.error(fd, msg)
	close_fd(fd)
	skynet.send(watchdog, "lua", "socket", "error", fd, msg)
end

-- 待发送的数据累计超过 1MB 的时候调用
-- @param fd socket id
-- @param size 累计的数据大小
function handler.warning(fd, size)
	skynet.send(watchdog, "lua", "socket", "warning", fd, size)
end

local CMD = {}

-- 设置 connection 对象的转发设置
-- @param source 请求服务源
-- @param fd socket id
-- @param client 模拟的发送源
-- @param address agent 服务
function CMD.forward(source, fd, client, address)
	local c = assert(connection[fd])
	unforward(c)
	c.client = client or 0
	c.agent = address or source
	forwarding[c.agent] = c
	gateserver.openclient(fd)
end

-- start socket, 但是未设置转发
-- @param source 请求服务源
-- @param fd socket id
function CMD.accept(source, fd)
	local c = assert(connection[fd])
	unforward(c)
	gateserver.openclient(fd)
end

-- 关闭 fd 连接
-- @param source 请求服务源
-- @param fd socket id
function CMD.kick(source, fd)
	gateserver.closeclient(fd)
end

-- 从其他服务接收到 lua 请求时调用
-- @param cmd 命令
-- @param source 请求服务源
-- @param ... 执行命令对应的函数
-- @return 对应命令的返回值
function handler.command(cmd, source, ...)
	local f = assert(CMD[cmd])
	return f(source, ...)
end

gateserver.start(handler)
