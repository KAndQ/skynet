-- 这个服务主要处理着与 master 的逻辑, 同时作为一个中间转发, 将消息传送给 service_harbor.c 服务.
-- 也处理着由 service_harbor.c 从各个 slave 节点接收并且转发过来的消息.

local skynet = require "skynet"
local socket = require "socket"
require "skynet.manager"	-- import skynet.launch, ...
local table = table

-- slave id 和 socket id 的映射表
local slaves = {}

-- 连接 slave 的队列, slave id 和主机地址的映射表集合
local connect_queue = {}

-- 服务全局名字和服务地址的映射表
local globalname = {}

-- 其他服务查询全局名字, 的响应函数的队列集合. 键是 name, 查询的全局名字, 值是回应函数队列集合
local queryname = {}

-- harbor 的命令函数处理集合
local harbor = {}

-- service_harbor.c 的 skyent_context handle
local harbor_service

-- 对 slave 节点的回应函数队列集合. 键是 slave id, 值是回应函数队列
local monitor = {}

-- 对 master 节点的回应函数集合
local monitor_master_set = {}

-- 从 socket 读取 1 个数据包, 然后将读取的数据解析成 lua 对象返回
local function read_package(fd)
	local sz = socket.read(fd, 1)
	assert(sz, "closed")
	sz = string.byte(sz)
	local content = assert(socket.read(fd, sz), "closed")
	return skynet.unpack(content)
end

-- 将 lua 对象打包成字符串的形式存储, 格式为: 数据长度 + 序列化数据
local function pack_package(...)
	local message = skynet.packstring(...)
	local size = #message
	assert(size <= 255 , "too long")
	return string.char(size) .. message
end

-- 回应 slave 客户端
-- @param id 回应的 slave id
local function monitor_clear(id)
	local v = monitor[id]
	if v then
		monitor[id] = nil
		for _, v in ipairs(v) do
			v(true)
		end
	end
end

-- 连接其他的 slave 主机
local function connect_slave(slave_id, address)
	local ok, err = pcall(function()
		if slaves[slave_id] == nil then
			-- socket 连接
			local fd = assert(socket.open(address), "Can't connect to "..address)
			skynet.error(string.format("Connect to harbor %d (fd=%d), %s", slave_id, fd, address))

			-- 记录连接的 slave id 和 socket id
			slaves[slave_id] = fd

			monitor_clear(slave_id)

			-- 将 socket id 转交 harbor 服务
			socket.abandon(fd)
			skynet.send(harbor_service, "harbor", string.format("S %d %d", fd, slave_id))
		end
	end)

	-- 错误报告
	if not ok then
		skynet.error(err)
	end
end

-- 当前的节点准备连接其他的节点
local function ready()
	local queue = connect_queue
	connect_queue = nil

	-- 连接 connect_queue 里面的 slave
	for k, v in pairs(queue) do
		connect_slave(k,v)
	end

	-- 向 harbor 服务更新全局名字
	for name, address in pairs(globalname) do
		skynet.redirect(harbor_service, address, "harbor", 0, "N " .. name)
	end
end

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

-- 和 master 连接的监控逻辑处理, 专门打开 1 个协程来处理.
local function monitor_master(master_fd)
	while true do
		local ok, t, id_name, address = pcall(read_package, master_fd)
		if ok then
			if t == 'C' then	-- 向 connect_queue 添加 slave id 和 address, 或者直接连接 slave
				if connect_queue then
					connect_queue[id_name] = address
				else
					connect_slave(id_name, address)
				end
			elseif t == 'N' then	-- 更新全局名字
				globalname[id_name] = address
				response_name(id_name)
				if connect_queue == nil then
					skynet.redirect(harbor_service, address, "harbor", 0, "N " .. id_name)
				end
			elseif t == 'D' then	-- 删除与 slave 的连接
				local fd = slaves[id_name]
				slaves[id_name] = false
				if fd then
					monitor_clear(id_name)
					socket.close(fd)
				end
			end
		else	-- 与 master 断开连接
			skynet.error("Master disconnect")
			for _, v in ipairs(monitor_master_set) do
				v(true)
			end
			socket.close(master_fd)
			break
		end
	end
end

-- 接受其他 slave 的连接
local function accept_slave(fd)
	socket.start(fd)

	-- 读取第 1 个字节
	local id = socket.read(fd, 1)
	if not id then
		skynet.error(string.format("Connection (fd = %d) closed", fd))
		socket.close(fd)
		return
	end

	-- 获得 slave id
	id = string.byte(id)
	if slaves[id] ~= nil then
		skynet.error(string.format("Slave %d exist (fd = %d)", id, fd))
		socket.close(fd)
		return
	end

	-- 记录 slave id 和 fd
	slaves[id] = fd

	-- 回应连接的 slave
	monitor_clear(id)

	-- 放弃当前 socket id, 并转交 socket id 给 harbor 服务
	socket.abandon(fd)
	skynet.error(string.format("Harbor %d connected (fd = %d)", id, fd))
	skynet.send(harbor_service, "harbor", string.format("A %d %d", fd, id))
end

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

-- 返回 1 个函数, 用于处理 text 类型的消息
local function monitor_harbor(master_fd)
	return function(session, source, command)
		-- 获得命令
		local t = string.sub(command, 1, 1)

		-- 获得参数
		local arg = string.sub(command, 3)
		if t == 'Q' then
			-- query name
			-- 查询名字
			if globalname[arg] then
				skynet.redirect(harbor_service, globalname[arg], "harbor", 0, "N " .. arg)
			else
				socket.write(master_fd, pack_package("Q", arg))
			end
		elseif t == 'D' then
			-- harbor down
			-- 关闭连接的 slave
			local id = tonumber(arg)
			if slaves[id] then
				monitor_clear(id)
			end
			slaves[id] = false
		else
			skynet.error("Unknown command ", command)
		end
	end
end

-- 给服务注册全局名字
-- @param fd socket id
-- @param name 全局名字
-- @param handle 服务地址
function harbor.REGISTER(fd, name, handle)
	-- 确保之前没有注册
	assert(globalname[name] == nil)

	-- 记录名字和服务地址
	globalname[name] = handle

	-- 响应之前的查询
	response_name(name)

	-- 给 master 发送信息
	socket.write(fd, pack_package("R", name, handle))

	-- 给 harbor 服务发送更新全局名字命令
	skynet.redirect(harbor_service, handle, "harbor", 0, "N " .. name)
end

-- 如果和 slave 连接, 那么将回应压入 monitor 队列, 否则直接发送回应.
function harbor.LINK(fd, id)
	if slaves[id] then
		if monitor[id] == nil then
			monitor[id] = {}
		end
		table.insert(monitor[id], skynet.response())
	else
		skynet.ret()
	end
end

-- 将回应压入 monitor_master_set
function harbor.LINKMASTER()
	table.insert(monitor_master_set, skynet.response())
end

-- 与 harbor.LINK 函数功能相同
function harbor.CONNECT(fd, id)
	if not slaves[id] then
		if monitor[id] == nil then
			monitor[id] = {}
		end
		table.insert(monitor[id], skynet.response())
	else
		skynet.ret()
	end
end

-- 查询服务, 如果是局部服务格式, 直接回应; 如果已经记录该全局名字, 直接回应; 否则将回应添加到查询队列中.
function harbor.QUERYNAME(fd, name)
	-- 局部名字直接回应
	if name:byte() == 46 then	-- "." , local name
		skynet.ret(skynet.pack(skynet.localname(name)))
		return
	end

	-- 之前已经记录, 则直接回应服务地址
	local result = globalname[name]
	if result then
		skynet.ret(skynet.pack(result))
		return
	end

	-- 还需要查询, 所以先将回应添加到集合队列中
	local queue = queryname[name]
	if queue == nil then
		queue = { skynet.response() }
		queryname[name] = queue
	else
		table.insert(queue, skynet.response())
	end
end

skynet.start(function()

	-- 获得 master 主机地址配置
	local master_addr = skynet.getenv "master"

	-- 获得 harbor id 配置
	local harbor_id = tonumber(skynet.getenv "harbor")

	-- 获得本机的侦听地址配置
	local slave_address = assert(skynet.getenv "address")

	-- 本机侦听
	local slave_fd = socket.listen(slave_address)

	-- 连接 master 主机
	skynet.error("slave connect to master " .. tostring(master_addr))
	local master_fd = assert(socket.open(master_addr), "Can't connect to master")

	-- 注册 lua 协议处理函数
	skynet.dispatch("lua", function (_,_,command,...)
		local f = assert(harbor[command])
		f(master_fd, ...)
	end)

	-- 注册 text 协议处理函数
	skynet.dispatch("text", monitor_harbor(master_fd))

	-- 启动 service_harbor.c 服务
	harbor_service = assert(skynet.launch("harbor", harbor_id, skynet.self()))

	-- 给 master 发送 H 命令
	local hs_message = pack_package("H", harbor_id, slave_address)
	socket.write(master_fd, hs_message)

	-- 拿到 master 的响应数据
	local t, n = read_package(master_fd)
	assert(t == "W" and type(n) == "number", "slave shakehand failed")

	skynet.error(string.format("Waiting for %d harbors", n))

	-- 开启 1 个协程, 专门处理当前节点与 master 的通信
	skynet.fork(monitor_master, master_fd)

	-- 等待其他的节点与当前节点连接
	if n > 0 then
		local co = coroutine.running()
		socket.start(slave_fd, function(fd, addr)
			skynet.error(string.format("New connection (fd = %d, %s)", fd, addr))
			if pcall(accept_slave, fd) then
				-- 统计连接的客户端数量
				local s = 0
				for k, v in pairs(slaves) do
					s = s + 1
				end

				-- 全部连接则唤醒当前协程
				if s >= n then
					skynet.wakeup(co)
				end
			end
		end)

		-- 阻塞, 等待全部的连接完成
		skynet.wait()
	end

	-- 关闭侦听节点, 其他节点无法连接当前节点, 但是当前节点可以主动去连接其他节点
	socket.close(slave_fd)

	skynet.error("Shakehand ready")

	-- 开启 1 协程, 用于连接其他 slave
	skynet.fork(ready)
end)
