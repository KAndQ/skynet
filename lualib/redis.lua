local skynet = require "skynet"
local socket = require "socket"
local socketchannel = require "socketchannel"

local table = table
local string = string

local redis = {}	-- 模块
local command = {}	-- 可以理解 redis 对象的具体实现
local meta = {	-- redis 对象的原表
	__index = command,
	-- DO NOT close channel in __gc
	-- 不要在 __gc 关闭 channel
}

---------- redis response
---------- redis 响应命令表
local redcmd = {}

-- 字符串回复, 以 $ 开头, 并在后面跟上字符串的长度, 并以 \r\n 分割, 接着是字符串内容和 \r\n
redcmd[36] = function(fd, data) -- '$'
	-- 拿到字符串长度
	local bytes = tonumber(data)
	if bytes < 0 then
		return true,nil
	end

	-- 读取的数据长度, 字符串长度 + 1(\r) + 1(\n)
	local firstline = fd:read(bytes+2)

	-- 读取字符串, 不要结尾的 \r\n
	return true,string.sub(firstline,1,-3)
end

-- 状态回复, 以 + 开头, 并在后面跟上状态信息, 最后以 \r\n 结尾
redcmd[43] = function(fd, data) -- '+'
	return true,data
end

-- 错误回复, 以 - 开头, 并在后面跟上错误信息, 最后以 \r\n 结尾
redcmd[45] = function(fd, data) -- '-'
	return false,data
end

-- 整数回复, 以 : 开头, 并在后面跟上数字, 最后以 \r\n 结尾
redcmd[58] = function(fd, data) -- ':'
	-- todo: return string later
	-- 待办: 稍后返回字符串
	return true, tonumber(data)
end

-- 读取回应, fd 是 channel.__sock 对象
local function read_response(fd)
	-- 首先读取回应数据格式是: 命令字符 + 内容 + \r\n
	-- 具体可以查看上面各个命令的格式介绍
	local result = fd:readline "\r\n"

	-- 得到第一个字符的内部编码
	local firstchar = string.byte(result)

	-- 得到命令字符后的内容字符串
	local data = string.sub(result,2)

	-- 返回实际的响应内容
	return redcmd[firstchar](fd,data)
end

-- 多行字符串回复, 以 * 开头, 并在后面跟上字符串回复的组数, 并以 \r\n 分隔
-- 例如: *3\r\n$1\r\n3\r\n$1\r\n2\r\n$1\r\n1\r\n
redcmd[42] = function(fd, data)	-- '*'
	-- 拿到字符串数组的数量
	local n = tonumber(data)
	if n < 0 then
		return true, nil
	end

	local bulk = {}
	local noerr = true
	for i = 1,n do
		local ok, v = read_response(fd)	-- 得到每组的字符串数据
		if ok then
			bulk[i] = v
		else
			noerr = false
		end
	end
	return noerr, bulk
end

-------------------

-- 认证函数, 登陆 redis
local function redis_login(auth, db)
	if auth == nil and db == nil then
		return
	end

	-- so 是 socketchannel
	return function(so)
		if auth then
			so:request("AUTH "..auth.."\r\n", read_response)
		end
		if db then
			so:request("SELECT "..db.."\r\n", read_response)
		end
	end
end

-- 连接到 redis 数据库, 连接成功返回可以操作 redis 的对象
function redis.connect(db_conf)
	local channel = socketchannel.channel {
		host = db_conf.host,
		port = db_conf.port or 6379,
		auth = redis_login(db_conf.auth, db_conf.db),
		nodelay = true,
	}

	-- try connect first only once
	-- 只尝试连接 1 次
	channel:connect(true)
	return setmetatable( { channel }, meta )
end

-- 断开与 redis 数据库的连接
function command:disconnect()
	self[1]:close()
	setmetatable(self, nil)
end

-- 插入 v, v 需要是字符串类型. 以 lines[i] = string.len(v), lines[i] = v 的格式插入 lines
local function pack_value(lines, v)
	if v == nil then
		return
	end

	v = tostring(v)

	table.insert(lines,"$"..#v)	-- 字符串长度
	table.insert(lines,v)	-- 字符串内容
end

-- 组合 cmd 和 msg 成字符串. 该字符串是 redis 的请求协议类型, 类似多行字符串回复格式.
-- cmd 字符串类型, msg nil/string/table/number 类型.
local function compose_message(cmd, msg)
	local len = 1
	local t = type(msg)

	-- 计算字符串的数量
	if t == "table" then
		len = len + #msg
	elseif t ~= nil then
		len = len + 1
	end

	-- 插入字符串数量和命令
	local lines = {"*" .. len}
	pack_value(lines, cmd)

	if t == "table" then	-- 插入 table 里面的值
		for _,v in ipairs(msg) do
			pack_value(lines, v)
		end
	else	-- 插入值
		pack_value(lines, msg)
	end
	table.insert(lines, "")

	local chunk =  table.concat(lines,"\r\n")	-- 使用 \r\n 将各个字符串分割
	return chunk
end

setmetatable(command, { __index = function(t,k)
	local cmd = string.upper(k)	-- 大写

	-- self 是 command table, v 
	local f = function (self, v, ...)
		if type(v) == "table" then
			return self[1]:request(compose_message(cmd, v), read_response)
		else
			return self[1]:request(compose_message(cmd, {v, ...}), read_response)
		end
	end

	t[k] = f	-- 记录 t[k], 下次再执行 t[k] 将不会再使用原表.

	return f
end})

-- 读取 boolean 值, 因为 redis 没有布尔类型, 以 1 表示 true, 0 表示 false
local function read_boolean(so)
	local ok, result = read_response(so)
	return ok, result ~= 0
end

-- redis.exists 返回 true/false
function command:exists(key)
	local fd = self[1]
	return fd:request(compose_message ("EXISTS", key), read_boolean)
end

-- redis.sismember 返回 true/false
function command:sismember(key, value)
	local fd = self[1]
	return fd:request(compose_message ("SISMEMBER", {key, value}), read_boolean)
end

--- watch mode
--- redis 的发布/订阅

local watch = {}	-- 可以理解为 redis 发布/订阅对象
local watchmeta = {
	__index = watch,
	__gc = function(self)
		self.__sock:close()
	end,
}

-- 登录认证
local function watch_login(obj, auth)
	return function(so)
		if auth then
			so:request("AUTH "..auth.."\r\n", read_response)
		end
		for k in pairs(obj.__psubscribe) do
			so:request(compose_message ("PSUBSCRIBE", k))
		end
		for k in pairs(obj.__subscribe) do
			so:request(compose_message("SUBSCRIBE", k))
		end
	end
end

-- 创建 redis 的发布/订阅对象
function redis.watch(db_conf)
	local obj = {
		__subscribe = {},	-- 订阅的频道, 键是订阅的频道
		__psubscribe = {},	-- 按规则订阅的频道, 键是订阅的频道模式
	}

	local channel = socketchannel.channel {
		host = db_conf.host,
		port = db_conf.port or 6379,
		auth = watch_login(obj, db_conf.auth),
		nodelay = true,
	}
	obj.__sock = channel

	-- try connect first only once
	-- 只尝试连接 1 次
	channel:connect(true)
	return setmetatable(obj, watchmeta)
end

-- 关闭连接
function watch:disconnect()
	self.__sock:close()
	setmetatable(self, nil)
end

-- 给 watch table 添加 name 函数
local function watch_func( name )
	local NAME = string.upper(name)	-- 大写

	watch[name] = function(self, ...)
		local so = self.__sock
		for i = 1, select("#", ...) do
			local v = select(i, ...)
			so:request(compose_message(NAME, v))	-- 只发送请求, 没有 response
		end
	end
end

watch_func "subscribe"		-- 给 watch 表添加 SUBSCRIBE 方法
watch_func "psubscribe"		-- 给 watch 表添加 PSUBSCRIBE 方法
watch_func "unsubscribe"	-- 给 watch 表添加 UNSUBSCRIBE 方法
watch_func "punsubscribe"	-- 给 watch 表添加 PUNSUBSCRIBE 方法

-- 侦听发布的消息
function watch:message()
	local so = self.__sock
	while true do
		local ret = so:response(read_response)
		local type , channel, data , data2 = ret[1], ret[2], ret[3], ret[4]
		if type == "message" then
			return data, channel		-- 消息内容, 接收频道
		elseif type == "pmessage" then
			return data2, data, channel	-- 消息内容, 接收频道, 订阅频道通配符
		elseif type == "subscribe" then
			self.__subscribe[channel] = true	-- 订阅成功
		elseif type == "psubscribe" then
			self.__psubscribe[channel] = true	-- 订阅成功
		elseif type == "unsubscribe" then
			self.__subscribe[channel] = nil		-- 取消订阅成功
		elseif type == "punsubscribe" then
			self.__psubscribe[channel] = nil	-- 取消订阅成功
		end
	end
end

return redis
