local skynet = require "skynet"
local core = require "skynet.core"
require "skynet.manager"	-- import manager apis
local string = string

-- 记录当前节点启动的服务, 键是服务地址(整型), 值是启动时的参数字符串
local services = {}

-- for confirm (function command.LAUNCH / command.ERROR / command.LAUNCHOK)
-- 用于确认 (函数 command.LAUNCH / command.ERROR / command.LAUNCHOK)
-- 键是服务地址(整型), 值是响应函数
local instance = {}

-- 处理函数集合
local command = {}

-- 将 :12345 格式字符串转换成整型
local function handle_to_address(handle)
	return tonumber("0x" .. string.sub(handle , 2))
end

-- 一个比较的返回值
local NORET = {}

-- 返回当前已经启动的服务器列表, 键是服务器地址, 值是启动参数; 相当于 services 的一个副本.
function command.LIST()
	local list = {}
	for k,v in pairs(services) do
		list[skynet.address(k)] = v
	end
	return list
end

-- 得到各个服务的状态列表, 键是服务地址, 值是状态的 table
function command.STAT()
	local list = {}
	for k,v in pairs(services) do
		local stat = skynet.call(k,"debug","STAT")
		list[skynet.address(k)] = stat
	end
	return list
end

-- 关闭掉某个服务, 返回关闭服务的 table, 键是服务地址, 值是服务和其启动参数
function command.KILL(_, handle)
	handle = handle_to_address(handle)
	skynet.kill(handle)
	local ret = { [skynet.address(handle)] = tostring(services[handle]) }
	services[handle] = nil
	return ret
end

-- 获得每个服务的内存信息, 返回 table 类型, 键是服务地址, 值是内存描述字符串
function command.MEM()
	local list = {}
	for k,v in pairs(services) do
		local kb, bytes = skynet.call(k,"debug","MEM")
		list[skynet.address(k)] = string.format("%.2f Kb (%s)",kb,v)
	end
	return list
end

-- 执行垃圾回收, 返回 command.MEM() 的结果
function command.GC()
	for k,v in pairs(services) do
		skynet.send(k,"debug","GC")
	end
	return command.MEM()
end

-- 移除服务
function command.REMOVE(_, handle, kill)
	services[handle] = nil
	local response = instance[handle]
	if response then	-- 这种情况是当在初始化未完成的时候会出现
		-- instance is dead
		-- 实例被废弃
		response(not kill)	-- return nil to caller of newservice, when kill == false, 当 kill == false 时, 给 newservice 的调用者返回 nil
		instance[handle] = nil
	end

	-- don't return (skynet.ret) because the handle may exit
	-- 不返回 (skynet.ret) 因为 handle 可能退出.
	return NORET
end

-- 启动服务, 返回启动的 handle
local function launch_service(service, ...)
	local param = table.concat({...}, " ")
	local inst = skynet.launch(service, param)
	local response = skynet.response()	-- 记住需要回应的服务
	if inst then
		services[inst] = service .. " " .. param
		instance[inst] = response
	else
		response(false)
		return
	end
	return inst
end

-- 启动服务
function command.LAUNCH(_, service, ...)
	launch_service(service, ...)
	return NORET
end

-- 启动服务, 并且同时打开服务的日志文件
function command.LOGLAUNCH(_, service, ...)
	local inst = launch_service(service, ...)
	if inst then
		core.command("LOGON", skynet.address(inst))
	end
	return NORET
end

-- 错误处理, 一般会在 skynet.init_service 中调用
function command.ERROR(address)
	-- see serivce-src/service_snlua.c
	-- init failed
	local response = instance[address]
	if response then
		response(false)
		instance[address] = nil
	end
	services[address] = nil
	return NORET
end

-- 创建服务成功, 一般会在 skynet.init_service 中调用
function command.LAUNCHOK(address)
	-- init notice
	local response = instance[address]
	if response then
		response(true, address)
		instance[address] = nil
	end

	return NORET
end

-- for historical reasons, launcher support text command (for C service)
-- 由于历史原因, launcher 支持文本命令(用于 C 服务)
skynet.register_protocol {
	name = "text",
	id = skynet.PTYPE_TEXT,
	unpack = skynet.tostring,
	dispatch = function(session, address, cmd)
		if cmd == "" then
			command.LAUNCHOK(address)
		elseif cmd == "ERROR" then
			command.ERROR(address)
		else
			error ("Invalid text command " .. cmd)
		end
	end,
}

-- 注册 "lua" 类型消息的处理函数
skynet.dispatch("lua", function(session, address, cmd , ...)
	cmd = string.upper(cmd)
	local f = command[cmd]
	if f then
		local ret = f(address, ...)
		if ret ~= NORET then
			skynet.ret(skynet.pack(ret))	-- 回应请求
		end
	else
		skynet.ret(skynet.pack {"Unknown command"} )
	end
end)

skynet.start(function() end)
