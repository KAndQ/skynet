-- snax 服务中可以定义一组函数用于响应其它服务提出的请求，并给出（或不给出）回应。一个非 snax 服务也可以调用 snax 服务的远程方法。

-- 要定义这类远程方法，可以通过定义 function response.foobar(...) 来声明一个远程方法。
-- foobar 是方法名，response 前缀表示这个方法一定有一个回应。你可以通过函数返回值来回应远程调用。

-- 调用这个远程方法，可以通过 obj.req.foobar(...) 来调用它。obj 是服务对象，req 表示这个调用需要接收返回值。
-- foobar 是方法的名字。其实，在创建出 obj 对象时，框架已经了解了这个 foobar 方法有返回值，这里多此一举让使用者
-- 明确 req 类型是为了让调用者（以及潜在的代码维护者）清楚这次调用是阻塞的，会导致服务挂起，等待对方的回应。

-- 如果你设计的协议不需要返回值，那么可以通过定义 function accept.foobar(...) 来声明。
-- 这里可以有和 response 组相同的方法名。通过 accept 前缀来区分 response 组下同名的方法。
-- 这类方法的实现函数不可以有返回值。

-- 调用这类不需要返回值的远程方法，应该使用 obj.post.foobar(...) 。这个调用不会阻塞。
-- 所以你也不用担心这里服务会挂起，因为别的消息进入改变了服务的内部状态。

-- 注: post 不适用于 Cluster 模式, 只能用于本地消息或 master/slave 结构下的消息。

local skynet = require "skynet"
local snax_interface = require "snax.interface"

-- snax 模块表
local snax = {}

-- snax 服务信息的缓存, 键是 snax 服务名, 值是 snax 服务信息对象
local typeclass = {}

-- 决定加载 snax 服务的时候使用的环境
local interface_g = skynet.getenv("snax_interface_g")
local G = interface_g and require (interface_g) or { require = function() end }
interface_g = nil

-- 注册 snax 协议
skynet.register_protocol {
	name = "snax",
	id = skynet.PTYPE_SNAX,
	pack = skynet.pack,
	unpack = skynet.unpack,
}

-- 加载 snax 服务, 存在 snax 服务的信息, 并且返回该服务信息.
-- @param name snax 服务名
-- @return table 服务的信息对象
function snax.interface(name)
	-- 加载过不再加载
	if typeclass[name] then
		return typeclass[name]
	end

	-- 实际的加载
	local si = snax_interface(name, G)

	-- 获得 snax 服务的处理函数
	local ret = {
		name = name,	-- snax 服务名
		accept = {},	-- snax 服务 accept 函数集合
		response = {},	-- snax 服务 response 函数集合
		system = {},	-- snax 服务 system 函数集合
	}

	for _,v in ipairs(si) do
		local id, group, name, f = table.unpack(v)
		ret[group][name] = id
	end

	-- 保存加载的服务
	typeclass[name] = ret
	return ret
end

local meta = { __tostring = function(v) return string.format("[%s:%x]", v.type, v.handle) end}

local skynet_send = skynet.send
local skynet_call = skynet.call

-- 封装 post 功能, 只发送. 可以参考上面的介绍.
-- @param type typeclass 的值
-- @param handle skynet_context handle, 即 snax 服务的 skynet 服务地址。
-- @return table, 该 table 比较特殊, 例如: t.name(...) 表示将 "name" 和 ... 作为参数发送给 handle 对应的服务
local function gen_post(type, handle)
	return setmetatable({} , {
		__index = function(t, k)
			local id = type.accept[k]
			if not id then
				error(string.format("post %s:%s no exist", type.name, k))
			end
			return function(...)
				skynet_send(handle, "snax", id, ...)
			end
		end })
end

-- 封装 request 功能, 发送并且等待响应. 可以参考上面的介绍.
-- @param type typeclass 的值
-- @param handle skynet_context handle, 即 snax 服务的 skynet 服务地址。
-- @return table, 该 table 比较特殊, 例如: local v = t.name(...) 表示将 "name" 和 ... 作为参数发送给 handle 对应的服务, 并且阻塞等待对应的服务响应.
local function gen_req(type, handle)
	return setmetatable({} , {
		__index = function(t, k)
			local id = type.response[k]
			if not id then
				error(string.format("request %s:%s no exist", type.name, k))
			end
			return function(...)
				return skynet_call(handle, "snax", id, ...)
			end
		end })
end

-- 生成 1 个 snax 服务对象
-- @param handle 即 snax 服务的 skynet 服务地址。
-- @param name snax 服务名
-- @param type typeclass 的值 
-- @return snax 服务对象
local function wrapper(handle, name, type)
	return setmetatable ({
		post = gen_post(type, handle),
		req = gen_req(type, handle),
		type = name,
		handle = handle,
		}, meta)
end

-- snax 服务对象的缓存表, 注意是弱表
local handle_cache = setmetatable( {} , { __mode = "kv" } )

-- 启动 1 个 snaxd 服务
-- @param name snax 服务名
-- @param ... snax 服务 init 使用的参数
-- @return snaxd 服务的 skynet_context handle
function snax.rawnewservice(name, ...)
	-- 得到服务信息
	local t = snax.interface(name)

	-- 启动 snaxd 服务
	local handle = skynet.newservice("snaxd", name)

	-- 确保 skynet_context 服务之前没有启动过
	assert(handle_cache[handle] == nil)

	-- snax 服务初始化
	if t.system.init then
		skynet.call(handle, "snax", t.system.init, ...)
	end

	return handle
end

-- 生成 snax 服务对象, 这样就可以和 snaxd 服务通信
-- @param handle skynet_context handle, 即 snax 服务的 skynet 服务地址。
-- @param type snax 服务名
-- @return snax 服务对象
function snax.bind(handle, type)
	local ret = handle_cache[handle]
	if ret then
		assert(ret.type == type)
		return ret
	end

	-- 生成 snax 服务信息对象
	local t = snax.interface(type)

	-- 生成 snax 服务对象, 并缓存起来
	ret = wrapper(handle, type, t)
	handle_cache[handle] = ret
	return ret
end

-- 传入服务名和参数，它会返回一个对象，用于和这个启动的服务交互。
-- 如果多次调用 newservice ，即使名字相同，也会生成多份服务的实例，它们各自独立，由不同的对象区分。
-- @param name snax 服务名
-- @param ... snax 服务初始化参数
-- @return snax 服务对象
function snax.newservice(name, ...)
	local handle = snax.rawnewservice(name, ...)
	return snax.bind(handle, name)
end

-- 该函数目前没有使用
local function service_name(global, name, ...)
	if global == true then
		return name
	else
		return global
	end
end

-- 但在一个节点上只会启动一份同名服务。如果你多次调用它，会返回相同的对象。
function snax.uniqueservice(name, ...)
	local handle = assert(skynet.call(".service", "lua", "LAUNCH", "snaxd", name, ...))
	return snax.bind(handle, name)
end

-- 但在整个 skynet 网络中（如果你启动了多个节点），只会有一个同名服务。
function snax.globalservice(name, ...)
	local handle = assert(skynet.call(".service", "lua", "GLAUNCH", "snaxd", name, ...))
	return snax.bind(handle, name)
end

-- 查询当前节点的具名服务，返回一个服务对象。如果服务尚未启动，那么一直阻塞等待它启动完毕。
function snax.queryservice(name)
	local handle = assert(skynet.call(".service", "lua", "QUERY", "snaxd", name))
	return snax.bind(handle, name)
end

-- 查询一个全局名字的服务，返回一个服务对象。如果服务尚未启动，那么一直阻塞等待它启动完毕。
function snax.queryglobal(name)
	local handle = assert(skynet.call(".service", "lua", "GQUERY", "snaxd", name))
	return snax.bind(handle, name)
end

-- 让一个 snax 服务退出
-- @param obj snax 服务对象
-- @param ... 运行 system.exit 函数传入的参数
function snax.kill(obj, ...)
	local t = snax.interface(obj.type)
	skynet_call(obj.handle, "snax", t.system.exit, ...)
end

-- 用来获取自己这个服务对象，它等价于 snax.bind(skynet.self(), SERVER_NAME)
function snax.self()
	return snax.bind(skynet.self(), SERVICE_NAME)
end

-- 退出当前服务，它等价于 snax.kill(snax.self(), ...) 
-- @param ... 运行 system.exit 函数传入的参数
function snax.exit(...)
	snax.kill(snax.self(), ...)
end

-- 用户 hotfix 函数, 当 hotfix 成功则返回对应的返回值; 如果 hotfix 失败, 则抛出错误.
local function test_result(ok, ...)
	if ok then
		return ...
	else
		error(...)
	end
end

-- 热更新 snax 服务（只能热更新 snax 框架编写的 lua 服务）
-- @param obj snax 服务对象
-- @param source 热更新代码字符串
function snax.hotfix(obj, source, ...)
	local t = snax.interface(obj.type)
	return test_result(skynet_call(obj.handle, "snax", t.system.hotfix, source, ...))
end

-- 格式化打印字符串
-- @param fmt 格式化串
-- @param ... 用于格式化串的不定参数
function snax.printf(fmt, ...)
	skynet.error(string.format(fmt, ...))
end

return snax
