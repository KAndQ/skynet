local skynet = require "skynet"
require "skynet.manager"	-- import skynet.launch, ...

-- 全局名字和主机地址的映射表
local globalname = {}

-- 其他服务查询全局名字, 的响应函数的队列集合. 键是 name, 查询的全局名字, 值是回应函数队列集合
local queryname = {}

-- harbor 的命令函数处理集合
local harbor = {}

-- service_harbor.c 的 skyent_context handle
local harbor_service

-- 注册 harbor 协议
skynet.register_protocol {
	name = "harbor",
	id = skynet.PTYPE_HARBOR,
	pack = function(...) return ... end,
	unpack = skynet.tostring,
}

-- 注册 text 协议
skynet.register_protocol {
	name = "text",
	id = skynet.PTYPE_TEXT,
	pack = function(...) return ... end,
	unpack = skynet.tostring,
}

-- 将 name 对应的全局服务地址回应给正在等待查询的服务
local function response_name(name)
	local address = globalname[name]
	if queryname[name] then
		local tmp = queryname[name]
		queryname[name] = nil
		for _,resp in ipairs(tmp) do
			resp(true, address)
		end
	end
end

-- 给服务注册全局名字
-- @param fd socket id
-- @param name 全局名字
-- @param handle 服务地址
function harbor.REGISTER(name, handle)
	assert(globalname[name] == nil)
	globalname[name] = handle
	response_name(name)
	skynet.redirect(harbor_service, handle, "harbor", 0, "N " .. name)
end

-- 查询服务, 如果是局部服务格式, 直接回应; 如果已经记录该全局名字, 直接回应; 否则将回应添加到查询队列中.
-- 参考 cslave.lua 的相同方法注释
function harbor.QUERYNAME(name)
	if name:byte() == 46 then	-- "." , local name
		skynet.ret(skynet.pack(skynet.localname(name)))
		return
	end

	local result = globalname[name]
	if result then
		skynet.ret(skynet.pack(result))
		return
	end

	local queue = queryname[name]
	if queue == nil then
		queue = { skynet.response() }
		queryname[name] = queue
	else
		table.insert(queue, skynet.response())
	end
end

-- 直接回应, 本来应该是在对应的 slave 退出时才回应.
function harbor.LINK(id)
	skynet.ret()
end

-- 不支持...
function harbor.CONNECT(id)
	skynet.error("Can't connect to other harbor in single node mode")
end

skynet.start(function()
	local harbor_id = tonumber(skynet.getenv "harbor")
	assert(harbor_id == 0)

	skynet.dispatch("lua", function (session,source,command,...)
		local f = assert(harbor[command])
		f(...)
	end)

	skynet.dispatch("text", function(session,source,command)
		-- ignore all the command
		-- 忽略全部的命令
	end)

	harbor_service = assert(skynet.launch("harbor", harbor_id, skynet.self()))
end)
