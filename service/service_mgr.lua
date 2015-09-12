local skynet = require "skynet"
require "skynet.manager"	-- import skynet.register
local snax = require "snax"

-- 函数功能集
local cmd = {}

-- 该表的键是启动服务时的名字, 如果是 snax 服务, 那么命名的规则是 snaxd.启动服务名; 否则直接是服务名.
-- 注意如果是 skynet 全局的服务, 那么 skynet 在使用名字作为键存储的时候, 将服务的名字添加上 @ 前缀.
-- 值有三种类型, 分别表示不同的状态:
-- table 类型, 表示该服务正在被状态, table 里面存储的是阻塞的协程对象;
-- string 类型, 表示该服务创建失败, 存储的是错误消息;
-- number 类型, 表示该服务创建成功, 返回的是服务的 handle.
local service = {}

-- 执行 func 函数, 同时存储服务 handle, 返回服务的 handle
-- @param name 服务名
-- @param func 启动服务函数
-- @param ... 启动服务需要的参数
-- @return 启动服务成功返回服务 handle, 否则返回错误字符串
local function request(name, func, ...)
	local ok, handle = pcall(func, ...)
	local s = service[name]
	
	-- 确认现在是等待服务创建状态
	assert(type(s) == "table")
	
	if ok then	-- 成功, 记录 handle
		service[name] = handle
	else	-- 失败, 记录失败消息
		service[name] = tostring(handle)
	end

	-- 唤醒等待的协程
	for _, v in ipairs(s) do
		skynet.wakeup(v)
	end

	-- 启动服务成功返回服务 handle, 否则返回错误字符串
	if ok then
		return handle
	else
		error(tostring(handle))
	end
end

-- 获得服务的 handle, 如果服务不存在, 那么将创建服务, 已存在则直接返回
-- @param name 服务名
-- @param func 创建服务的函数
-- @param ... 创建服务需要的函数
-- @return 返回服务的 handle
local function waitfor(name, func, ...)
	local s = service[name]

	-- 服务已经创建成功, 直接返回 handle
	if type(s) == "number" then
		return s
	end

	if s == nil then					-- 进入创建服务状态
		s = {}
		service[name] = s
	elseif type(s) == "string" then		-- 服务之前创建失败, 保存的是错误信息
		error(s)
	end

	-- 确认是创建服务状态
	assert(type(s) == "table")

	-- 创建服务
	if not s.launch and func then
		s.launch = true	-- 标记正在创建服务
		return request(name, func, ...)
	end

	-- 阻塞当前协程, 等待创建成功后唤醒
	local co = coroutine.running()
	table.insert(s, co)
	skynet.wait()

	s = service[name]

	-- 判断是否服务创建失败
	if type(s) == "string" then
		error(s)
	end

	-- 确认服务创建成功保存的 handle 有效
	assert(type(s) == "number")

	return s
end

-- 读取服务名字, 得到服务的实际名字. 在存储的时候, 全网络唯一服务是带 @ 前缀的.
-- @param service_name 服务名字, 可能带有 @ 前缀
-- @return 返回服务的真实名字, 不带 @ 前缀
local function read_name(service_name)
	if string.byte(service_name) == 64 then -- '@'
		return string.sub(service_name , 2)
	else
		return service_name
	end
end

-- 启动服务
-- @param service_name 如果希望启动的是 snax 服务, 那么 service_name 为 "snaxd"; 否则是直接指定服务名
-- @param subname 如果希望启动的是 snax 服务, 那么 subname 就是 snax 服务名; 对于非 snax 服务, 只是作为 1 个参数来使用
-- @param ... 启动服务需要的参数
-- @return 服务的 handle
function cmd.LAUNCH(service_name, subname, ...)
	local realname = read_name(service_name)

	if realname == "snaxd" then
		return waitfor(service_name .. "." .. subname, snax.rawnewservice, subname, ...)
	else
		return waitfor(service_name, skynet.newservice, realname, subname, ...)
	end
end

-- 查询服务. 注意: 如果服务还未创建, 那么会阻塞, 等待服务创建成功才唤醒.
-- @param service_name 如果希望启动的是 snax 服务, 那么 service_name 为 "snaxd"; 否则是直接指定服务名
-- @param subname 如果希望启动的是 snax 服务, 那么 subname 就是 snax 服务名; 对于非 snax 服务, 该参数无效
-- @return 服务的 handle
function cmd.QUERY(service_name, subname)
	local realname = read_name(service_name)

	if realname == "snaxd" then
		return waitfor(service_name.."."..subname)
	else
		return waitfor(service_name)
	end
end

-- 返回服务列表
-- @return table
local function list_service()
	local result = {}
	for k, v in pairs(service) do
		if type(v) == "string" then
			v = "Error: " .. v
		elseif type(v) == "table" then
			v = "Querying"
		else
			v = skynet.address(v)
		end

		result[k] = v
	end

	return result
end

-- 如果当前节点是 master 节点, 那么将使用这个注册函数
local function register_global()

	-- 在当前节点启动 skynet 全局服务
	-- @param name 服务名字
	-- @param ... 启动服务参数
	-- @return 服务 handle
	function cmd.GLAUNCH(name, ...)
		local global_name = "@" .. name
		return cmd.LAUNCH(global_name, ...)
	end

	-- 在当前节点查询 skynet 全局服务
	-- @param name 服务名字
	-- @param ... 可选参数
	-- @return 服务 handle
	function cmd.GQUERY(name, ...)
		local global_name = "@" .. name
		return cmd.QUERY(global_name, ...)
	end

	-- 存储非 master 节点的 service_mgr(.service) 服务 handle
	local mgr = {}

	-- 记录非 master 节点的 service_mgr(.service) 服务 handle
	-- @param m 服务 handle
	function cmd.REPORT(m)
		mgr[m] = true
	end

	-- 从各个节点获得唯一服务的列表信息
	-- @param all table 类型, 将各个节点的唯一服务信息存在这个表里, { service_name@harborid = string }
	-- @param m 服务 handle
	local function add_list(all, m)
		-- 获得 skynet 节点 id
		local harbor = "@" .. skynet.harbor(m)
		local result = skynet.call(m, "lua", "LIST")
		for k, v in pairs(result) do
			all[k .. harbor] = v
		end
	end

	-- 获得当前 skynet 网络各个节点的唯一服务信息
	function cmd.LIST()
		local result = {}

		-- 从各个节点获得唯一服务列表
		for k in pairs(mgr) do
			pcall(add_list, result, k)
		end

		-- 当前节点唯一服务列表
		local l = list_service()
		for k, v in pairs(l) do
			result[k] = v
		end

		return result
	end
end

-- 如果当前节点是 slave 节点, 那么将使用这个注册函数
local function register_local()

	-- 启动 skynet 全局唯一服务
	-- @param name 服务名
	-- @param ... 启动服务需要参数
	-- @return 服务的 handle
	function cmd.GLAUNCH(name, ...)
		local global_name = "@" .. name
		return waitfor(global_name, skynet.call, "SERVICE", "lua", "LAUNCH", global_name, ...)
	end

	-- 查询 skynet 全局唯一服务, 如果服务还未创建, 当前协程会阻塞, 直到服务创建成功才会返回
	-- @param name 服务名
	-- @param ... 可选参数
	-- @return 服务的 handel
	function cmd.GQUERY(name, ...)
		local global_name = "@" .. name
		return waitfor(global_name, skynet.call, "SERVICE", "lua", "QUERY", global_name, ...)
	end

	-- 返回当前节点唯一服务列表
	function cmd.LIST()
		return list_service()
	end

	-- 报告给 master 当前节点 .service 服务的 handle
	skynet.call("SERVICE", "lua", "REPORT", skynet.self())
end

skynet.start(function()
	skynet.dispatch("lua", function(session, address, command, ...)
		local f = cmd[command]
		if f == nil then
			skynet.ret(skynet.pack(nil, "Invalid command " .. command))
			return
		end

		local ok, r = pcall(f, ...)

		if ok then
			skynet.ret(skynet.pack(r))
		else
			skynet.ret(skynet.pack(nil, r))
		end
	end)

	-- 注册 .service, 如果已经启动过该服务, 无法再次启动
	local handle = skynet.localname ".service"
	if handle then
		skynet.error(".service is already register by ", skynet.address(handle))
		skynet.exit()
	else
		skynet.register(".service")
	end

	-- 如果当前是 master 节点, 那么注册全局服务名 SERVICE, 其他节点通过和 master 节点的 SERVICE 服务来做全局服务管理
	if skynet.getenv "standalone" then
		skynet.register("SERVICE")
		register_global()
	else
		register_local()
	end
end)
