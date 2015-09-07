local skynet = require "skynet"
local socket = require "socket"

--[[
	master manage data :
		1. all the slaves address : id -> ipaddr:port
		2. all the global names : name -> address

	master 管理数据:
		1. 全部的 slaves 主机地址: id -> ipaddr:port
		2. 全部的全剧名字: name -> address

	master hold connections from slaves.
	master 控制来自节点的连接.

	protocol slave->master :
		package size 1 byte
		type 1 byte :
			'H' : HANDSHAKE, report slave id, and address.
			'R' : REGISTER name address
			'Q' : QUERY name
	slave->master 协议:
		包大小使用 1 字节表示
		1 字节表示命令类型
			'H' : 握手, 发送 slave id 和主机地址.
			'R' : 注册服务全局名字, 发送 name 和 handle
			'Q' : 查询全局名字

	protocol master->slave:
		package size 1 byte
		type 1 byte :
			'W' : WAIT n
			'C' : CONNECT slave_id slave_address
			'N' : NAME globalname address
			'D' : DISCONNECT slave_id
	master->slave 协议:
		包大小使用 1 字节表示
		1 字节表示命令类型
			'W' : 等待连接, 发送 n 表示需要等待连接节点的数量
			'C' : 连接节点, 发送 slave id 和 slave 主机地址
			'N' : 更新全局名字, 发送全局名字和服务地址
			'D' : 断开 slave 连接, 发送断开的 slave id
]]

-- 管理的 slave 节点
local slave_node = {}

-- 管理的服务的全局名字
local global_name = {}

-- 从 socket 读取 1 个数据包, 然后将读取的数据解析成 lua 对象返回, 详细参考 cslave.lua, 有相同的函数
local function read_package(fd)
	local sz = socket.read(fd, 1)
	assert(sz, "closed")
	sz = string.byte(sz)
	local content = assert(socket.read(fd, sz), "closed")
	return skynet.unpack(content)
end

-- 将 lua 对象打包成字符串的形式存储, 格式为: 数据长度 + 序列化数据, 详细参考 cslave.lua, 有相同的函数
local function pack_package(...)
	local message = skynet.packstring(...)
	local size = #message
	assert(size <= 255 , "too long")
	return string.char(size) .. message
end

local function report_slave(fd, slave_id, slave_addr)
	local message = pack_package("C", slave_id, slave_addr)
	local n = 0
	for k,v in pairs(slave_node) do
		if v.fd ~= 0 then
			socket.write(v.fd, message)
			n = n + 1
		end
	end
	socket.write(fd, pack_package("W", n))
end

local function handshake(fd)
	local t, slave_id, slave_addr = read_package(fd)
	assert(t=='H', "Invalid handshake type " .. t)
	assert(slave_id ~= 0 , "Invalid slave id 0")
	if slave_node[slave_id] then
		error(string.format("Slave %d already register on %s", slave_id, slave_node[slave_id].addr))
	end
	report_slave(fd, slave_id, slave_addr)
	slave_node[slave_id] = {
		fd = fd,
		id = slave_id,
		addr = slave_addr,
	}
	return slave_id , slave_addr
end

local function dispatch_slave(fd)
	local t, name, address = read_package(fd)
	if t == 'R' then
		-- register name
		assert(type(address)=="number", "Invalid request")
		if not global_name[name] then
			global_name[name] = address
		end
		local message = pack_package("N", name, address)
		for k,v in pairs(slave_node) do
			socket.write(v.fd, message)
		end
	elseif t == 'Q' then
		-- query name
		local address = global_name[name]
		if address then
			socket.write(fd, pack_package("N", name, address))
		end
	else
		skynet.error("Invalid slave message type " .. t)
	end
end

local function monitor_slave(slave_id, slave_address)
	local fd = slave_node[slave_id].fd
	skynet.error(string.format("Harbor %d (fd=%d) report %s", slave_id, fd, slave_address))
	while pcall(dispatch_slave, fd) do end
	skynet.error("slave " ..slave_id .. " is down")
	local message = pack_package("D", slave_id)
	slave_node[slave_id].fd = 0
	for k,v in pairs(slave_node) do
		socket.write(v.fd, message)
	end
	socket.close(fd)
end

skynet.start(function()
	local master_addr = skynet.getenv "standalone"
	skynet.error("master listen socket " .. tostring(master_addr))
	local fd = socket.listen(master_addr)
	socket.start(fd , function(id, addr)
		skynet.error("connect from " .. addr .. " " .. id)
		socket.start(id)
		local ok, slave, slave_addr = pcall(handshake, id)
		if ok then
			skynet.fork(monitor_slave, slave, slave_addr)
		else
			skynet.error(string.format("disconnect fd = %d, error = %s", id, slave))
			socket.close(id)
		end
	end)
end)
