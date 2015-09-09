-- 这个服务只会在 master 节点启动. datacenter 其实是通过在 master 节点上部署了一个专门的数据中心服务来共享这些数据的。
-- datacenter 服务支持的数据存储结构是, key1.key2.key3, 例如在查询的时候, 传入的键的参数个数是 key1, key2, key3, 
-- 这时能够得到 key1.key2.key3 的值.

-- datacenter 类似一个全网络共享的注册表。它是一个树结构，任何人都可以向其中写入一些合法的 lua 数据，其它服务可以从中取出来。
-- 所以你可以把一些需要跨节点访问的服务，自己把其地址记在 datacenter 中，需要的人可以读出。是 1 个树形结构.

local skynet = require "skynet"

-- 处理函数集合表
local command = {}

-- 存储数据的数据库, 存储的值可能是 table 类型; 如果是 table 类型, 那么将作为 1 个子数据库使用.
local database = {}

-- 等待更新的查询队列
local wait_queue = {}

-- 作为 1 个键使用
local mode = {}

-- 从 db 里面查询数据, 如果 key 为 nil, 直接返回 db 值. 否则根据参数递归查询.
local function query(db, key, ...)
	if key == nil then
		return db
	else
		return query(db[key], ...)
	end
end

-- 获得查询的值.
-- 没有查询到返回 nil, 否则返回查询到的值.
function command.QUERY(key, ...)
	local d = database[key]
	if d then
		return query(d, ...)
	end
end

-- 更新键值, 第一个参数是 db(table 类型), 最后一个参数将作为值存储. 生成类似 key1.key2.key3 的键结构.
-- 返回两个数据, 第一个是更新前的值, 第二个是更新后的值.
local function update(db, key, value, ...)
	if select("#", ...) == 0 then
		local ret = db[key]
		db[key] = value
		return ret, value
	else
		if db[key] == nil then
			db[key] = {}
		end
		return update(db[key], value, ...)
	end
end

-- 响应正在等待的查询
-- @param db wait_queue
-- @return 等待响应函数的集合
local function wakeup(db, key1, ...)
	-- 传入的键值不正确
	if key1 == nil then
		return
	end

	-- 还未有 key1 的值写入
	local q = db[key1]
	if q == nil then
		return
	end

	if q[mode] == "queue" then	-- 叶子节点
		db[key1] = nil
		if select("#", ...) ~= 1 then	-- 因为最后 1 个参数是值, 如果大于 1, 说明还不是倒数第二个参数, 但是却标记了 "queue", 所以要报告错误
			-- throw error because can't wake up a branch
			-- 抛出一个错误, 因为无法唤醒 1 个分支
			for _, response in ipairs(q) do
				response(false)
			end
		else
			return q
		end
	else
		-- it's branch
		-- 它的分支, 继续向下递归查询
		return wakeup(q, ...)
	end
end

-- 更新数据, 并且响应对待的服务
-- @return 如果更新前存在值, 则返回更新前的值; 否则返回 nil.
function command.UPDATE(...)
	local ret, value = update(database, ...)
	if ret or value == nil then
		return ret
	end

	-- 获得等待响应的函数集合
	local q = wakeup(wait_queue, ...)

	-- 响应等待的服务
	if q then
		for _, response in ipairs(q) do
			response(true, value)
		end
	end
end

-- 将查询键的响应添加到等待查询的队列
-- @param db wait_queue
-- @return nil
local function waitfor(db, key1, key2, ...)
	if key2 == nil then
		-- push queue
		local q = db[key1]
		if q == nil then
			q = { [mode] = "queue" }	-- 标记为 1 个叶子节点
			db[key1] = q
		else
			assert(q[mode] == "queue")
		end
		table.insert(q, skynet.response())
	else
		local q = db[key1]
		if q == nil then
			q = { [mode] = "branch" }	-- 标记为 1 个分支节点
			db[key1] = q
		else
			assert(q[mode] == "branch")
		end
		return waitfor(q, key2, ...)
	end
end

skynet.start(function()
	-- 注册 lua 类型的处理函数
	skynet.dispatch("lua", function (_, _, cmd, ...)
		if cmd == "WAIT" then
			local ret = command.QUERY(...)
			if ret then	-- 有值则直接响应
				skynet.ret(skynet.pack(ret))
			else	-- 没有值则压入队列, 等待更新之后响应
				waitfor(wait_queue, ...)
			end
		else
			local f = assert(command[cmd])
			skynet.ret(skynet.pack(f(...)))
		end
	end)
end)
