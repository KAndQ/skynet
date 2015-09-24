-- 实现 gate.lua 的 watchdog 服务

local skynet = require "skynet"

local CMD = {}	-- 对于 gate.lua 发送的基于 lua 协议非 "socket" 消息处理函数集合, 这里面函数的返回值需要回应给 gateserver 服务

local SOCKET = {}	-- 对于 gate.lua 发送的基于 lua 协议 "socket" 消息处理函数集合, 这里面函数的返回值是不需要回应给 gateserver 服务

local gate	-- gateserver 服务 handler
local agent = {}	-- socket id -> agent handler 的映射表

-- 在 handler.connect 时会发送这个 lua 消息, 新开启 1 个 agent 服务.
-- @param fd socket id 
-- @param addr 远程终端的主机地址
function SOCKET.open(fd, addr)
	skynet.error("New client from : " .. addr)
	agent[fd] = skynet.newservice("agent")
	skynet.call(agent[fd], "lua", "start", { gate = gate, client = fd, watchdog = skynet.self() })
end

-- 关闭 fd 连接和 fd 对应的 agent 服务
-- @param fd socket id
local function close_agent(fd)
	local a = agent[fd]
	agent[fd] = nil
	if a then
		skynet.call(gate, "lua", "kick", fd)	-- 关闭 fd 的 socket 连接
		
		-- disconnect never return
		-- disconnect 命令不需要返回
		skynet.send(a, "lua", "disconnect")		-- 关闭 agent 服务
	end
end

-- 关闭 agent
-- @param fd socket id
function SOCKET.close(fd)
	print("socket close", fd)
	close_agent(fd)
end

-- socket 出错
-- @param fd socket id
-- @param msg 错误信息
function SOCKET.error(fd, msg)
	print("socket error",fd, msg)
	close_agent(fd)
end

-- socket 警告
-- @param fd socket id
-- @param size 未发出数据的大小
function SOCKET.warning(fd, size)
	-- size K bytes havn't send out in fd
	-- K 字节大小的数据尚未发出
	print("socket warning", fd, size)
end

-- handler.message 如果无转发 agent 的时候会收到
-- @param fd socket id
-- @param msg string 类型, 通过 netpack.tostring 转给 lua 来处理
function SOCKET.data(fd, msg)
end

-- 让 gateserver 服务开始侦听, 执行 gateserver 的 CMD.open 方法.
-- @param conf 开启 gateserver 的配置表
function CMD.start(conf)
	skynet.call(gate, "lua", "open", conf)
end

-- 关闭 fd 连接, 同时关闭 agent 服务
-- @param fd socket id
function CMD.close(fd)
	close_agent(fd)
end

skynet.start(function()
	skynet.dispatch("lua", function(session, source, cmd, subcmd, ...)
		if cmd == "socket" then
			local f = SOCKET[subcmd]
			f(...)
			-- socket api don't need return
			-- socket api 不需要返回
		else
			local f = assert(CMD[cmd])
			skynet.ret(skynet.pack(f(subcmd, ...)))
		end
	end)

	gate = skynet.newservice("gate")
end)
