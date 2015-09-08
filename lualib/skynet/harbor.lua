local skynet = require "skynet"

local harbor = {}

-- 给服务注册全局名字
-- @param name 全局名字
-- @param handle 服务 handle
function harbor.globalname(name, handle)
	handle = handle or skynet.self()
	skynet.send(".cslave", "lua", "REGISTER", name, handle)
end

-- 查询服务名字, 返回服务地址
function harbor.queryname(name)
	return skynet.call(".cslave", "lua", "QUERYNAME", name)
end

-- 阻塞当前协程, 当 id 对应的 slave 断开连接的时候才唤醒该协程
function harbor.link(id)
	skynet.call(".cslave", "lua", "LINK", id)
end

-- 阻塞当前协程, 当 id 对应的 slave 连接的时候才唤醒该协程
function harbor.connect(id)
	skynet.call(".cslave", "lua", "CONNECT", id)
end

-- 让 cslave 服务在关闭与 master 连接的时候回应. 当调用这个函数的时候当前协程会一直阻塞, 直到 cslave 与 master 断开连接并且发送回应后才会唤醒阻塞协程.
function harbor.linkmaster()
	skynet.call(".cslave", "lua", "LINKMASTER")
end

return harbor
