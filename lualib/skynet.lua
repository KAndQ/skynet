local c = require "skynet.core"
local tostring = tostring
local tonumber = tonumber
local coroutine = coroutine
local assert = assert
local pairs = pairs
local pcall = pcall

local profile = require "profile"
coroutine.resume = profile.resume
coroutine.yield = profile.yield

-- 原型记录, 记录 proto[class.name] = class; proto[class.id] = class;
local proto = {}

local skynet = {
	-- read skynet.h, 参考 skynet.h 的消息类别定义
	PTYPE_TEXT = 0,
	PTYPE_RESPONSE = 1,
	PTYPE_MULTICAST = 2,
	PTYPE_CLIENT = 3,
	PTYPE_SYSTEM = 4,
	PTYPE_HARBOR = 5,
	PTYPE_SOCKET = 6,
	PTYPE_ERROR = 7,
	PTYPE_QUEUE = 8,	-- used in deprecated mqueue, use skynet.queue instead, 不赞成使用 mqueue, 建议使用 skynet.queue 代替
	PTYPE_DEBUG = 9,
	PTYPE_LUA = 10,
	PTYPE_SNAX = 11,
}

-- code cache, 在 service_snlua.c 中初始化
skynet.cache = require "skynet.codecache"

--[[
例如你可以注册一个以文本方式编码消息的消息类别。通常用 C 编写的服务更容易解析文本消息。
skynet 已经定义了这种消息类别为 skynet.PTYPE_TEXT，但默认并没有注册到 lua 中使用。

class = {
  name = "text",
  id = skynet.PTYPE_TEXT,
  pack = function(m) return tostring(m) end,
  unpack = skynet.tostring,
  dispatch = dispatch,	-- 下面说到的 dispatch 函数.
}

新的类别必须提供 pack 和 unpack 函数，用于消息的编码和解码。

pack 函数必须返回一个 string 或是一个 userdata 和 size。
在 Lua 脚本中，推荐你返回 string 类型，而用后一种形式需要对 skynet 底层有足够的了解（采用它多半是因为性能考虑，可以减少一些数据拷贝）。

unpack 函数接收一个 lightuserdata 和一个整数 。即上面提到的 message 和 size。
lua 无法直接处理 C 指针，所以必须使用额外的 C 库导入函数来解码。skynet.tostring 就是这样的一个函数，它将这个 C 指针和长度翻译成 lua 的 string。

接下来你可以使用 skynet.dispatch 注册 text 类别的处理方法了。当然，直接在 skynet.register_protocol 时传入 dispatch 函数也可以。
--]]

-- 在 skynet 中注册新的消息类别.
-- @param class 里面的字段参考上面的注释
-- @return nil
function skynet.register_protocol(class)
	local name = class.name
	local id = class.id
	assert(proto[name] == nil)	-- 保证之前没有注册过
	assert(type(name) == "string" and type(id) == "number" and id >=0 and id <= 255)	-- 保证类型正确, 并且值有效
	proto[name] = class
	proto[id] = class
end

-- session 和 coroutine 的映射关系表, 键是 session, 值是 coroutine
local session_id_coroutine = {}

-- coroutine 和 session 的映射关系表, 键是 coroutine, 值是 session
local session_coroutine_id = {}

-- coroutine 和 address 的映射关系表, 键是 coroutine, 值是 address
local session_coroutine_address = {}

local session_response = {}
local unresponse = {}

-- 记录需要唤醒的 coroutine, 键是 coroutine, 值是 true
local wakeup_session = {}

-- 当前因 "SLEEP" 挂起的协程, 键是 session, 值是 coroutine
local sleep_session = {}

-- 监控的服务, 键是服务地址, 值是该服务的引用计数
local watching_service = {}

-- 监控的 session, 键是 session, 值是服务地址
local watching_session = {}

-- 
local dead_service = {}

-- 错误队列, 存放的是 session 的数组
local error_queue = {}

-- 通过 skynet.fork() 创建的 coroutine 的集合
local fork_queue = {}

-- suspend is function
-- suspend 是函数
local suspend

-- 将 :ff1234 形式的字符串转化成 0xff1234 这种格式的字符串, 然后再转化成整型返回.
local function string_to_handle(str)
	return tonumber("0x" .. string.sub(str , 2))
end

----- monitor exit
----- 监视器退出

-- 从错误队列派发错误
local function dispatch_error_queue()
	local session = table.remove(error_queue,1)
	if session then
		local co = session_id_coroutine[session]
		session_id_coroutine[session] = nil
		return suspend(co, coroutine.resume(co, false))
	end
end

-- 将 error_session 压入到 error_queue 中
local function _error_dispatch(error_session, error_source)
	if error_session == 0 then
		-- service is down
		-- Don't remove from watching_service , because user may call dead service
		-- 服务牺牲了
		-- 不要从 watching_service 移除, 因为用户可能调用死掉的服务
		if watching_service[error_source] then
			dead_service[error_source] = true
		end

		for session, srv in pairs(watching_session) do
			if srv == error_source then
				table.insert(error_queue, session)
			end
		end
	else
		-- capture an error for error_session
		-- 捕获 1 个错误
		if watching_session[error_session] then
			table.insert(error_queue, error_session)
		end
	end
end

-- coroutine reuse
-- 协程复用

local coroutine_pool = {}	-- 协程池, 存放着当前可用的协程对象
local coroutine_yield = coroutine.yield

-- 获得 1 个协程对象
local function co_create(f)
	local co = table.remove(coroutine_pool)
	if co == nil then
		co = coroutine.create(function(...)
			f(...)	-- 执行协程的主函数逻辑
			while true do
				f = nil
				coroutine_pool[#coroutine_pool+1] = co	-- 记录此协程 co 可用
				f = coroutine_yield "EXIT"	-- 让出, 通知此协程退出, 删除相关资源; 恢复执行的时候得到新的执行函数. #2
				f(coroutine_yield())	-- 再次让出, 等待传来参数; 恢复的时候继续执行函数主体. #1
			end
		end)
	else
		coroutine.resume(co, f)	-- 如果此 co 是从池中取出, 那么恢复此 co, 关联到上面的代码的 #2 位置
	end
	return co
end

-- 唤醒 sleep 的 coroutine
local function dispatch_wakeup()
	local co = next(wakeup_session)
	if co then
		wakeup_session[co] = nil
		local session = sleep_session[co]
		if session then
			session_id_coroutine[session] = "BREAK"
			return suspend(co, coroutine.resume(co, false, "BREAK"))
		end
	end
end

-- 释放监控服务的引用
local function release_watching(address)
	local ref = watching_service[address]
	if ref then
		ref = ref - 1
		if ref > 0 then
			watching_service[address] = ref
		else
			watching_service[address] = nil
		end
	end
end

-- suspend is local function
-- syspend 是一个私有函数
--[====[
result 如果为 false, 那么表示该 co 发生了错误;
command 命令, 各个详细的命令在下面说明;
param 其他参数;
size 这个和 param 有关, 如果 param 是数据指针, 那么 size 表示指针所指数据的大小;
--]====]
function suspend(co, result, command, param, size)
	-- 如果协程运行发生错误, 发送错误信息给消息源
	if not result then
		-- 拿到保存的 session 
		local session = session_coroutine_id[co]
		if session then -- coroutine may fork by others (session is nil), co 可能被其他协程 fork(session 为 nil)
			local addr = session_coroutine_address[co]
			if session ~= 0 then
				-- only call response error
				-- 告诉请求服务, 产生错误了
				c.send(addr, skynet.PTYPE_ERROR, session, "")
			end
			session_coroutine_id[co] = nil
			session_coroutine_address[co] = nil
		end
		error(debug.traceback(co,tostring(command)))
	end

	if command == "CALL" then
		session_id_coroutine[param] = co
	elseif command == "SLEEP" then
		session_id_coroutine[param] = co
		sleep_session[co] = param
	elseif command == "RETURN" then
		local co_session = session_coroutine_id[co]
		local co_address = session_coroutine_address[co]
		if param == nil or session_response[co] then
			error(debug.traceback(co))
		end

		session_response[co] = true

		local ret
		if not dead_service[co_address] then
			ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, param, size) ~= nil
			if not ret then
				-- If the package is too large, returns nil. so we should report error back
				-- 如果数据包太大, 返回 nil. 所以我们应该回应错误.
				c.send(co_address, skynet.PTYPE_ERROR, co_session, "")
			end
		elseif size ~= nil then
			c.trash(param, size)
			ret = false
		end

		return suspend(co, coroutine.resume(co, ret))
	elseif command == "RESPONSE" then
		local co_session = session_coroutine_id[co]
		local co_address = session_coroutine_address[co]
		
		if session_response[co] then
			error(debug.traceback(co))
		end

		local f = param
		local function response(ok, ...)
			if ok == "TEST" then
				if dead_service[co_address] then
					release_watching(co_address)
					f = false
					return false
				else
					return true
				end
			end

			if not f then
				if f == false then
					f = nil
					return false
				end
				error "Can't response more than once"
			end

			local ret
			if not dead_service[co_address] then
				if ok then
					ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, f(...)) ~= nil
					if not ret then
						-- If the package is too large, returns false. so we should report error back
						-- 如果包太大, 返回 nil. 所以我们应该回应错误.
						c.send(co_address, skynet.PTYPE_ERROR, co_session, "")
					end
				else
					ret = c.send(co_address, skynet.PTYPE_ERROR, co_session, "") ~= nil
				end
			else
				ret = false
			end

			release_watching(co_address)
			unresponse[response] = nil
			f = nil
			return ret
		end

		watching_service[co_address] = watching_service[co_address] + 1
		session_response[co] = response
		unresponse[response] = true
		return suspend(co, coroutine.resume(co, response))
	elseif command == "EXIT" then
		-- coroutine exit
		-- 协程退出
		local address = session_coroutine_address[co]
		release_watching(address)
		session_coroutine_id[co] = nil
		session_coroutine_address[co] = nil
		session_response[co] = nil
	elseif command == "QUIT" then
		-- service exit
		-- 服务退出
		return
	elseif command == nil then
		-- debug trace
		-- 调试追踪
		return
	else
		error("Unknown command : " .. command .. "\n" .. debug.traceback(co))
	end

	dispatch_wakeup()
	dispatch_error_queue()
end

-- 让框架在 ti 个单位时间后，调用 func 这个函数。这不是一个阻塞 API ，当前 coroutine 会继续向下运行，而 func 将来会在新的 coroutine 中执行。
function skynet.timeout(ti, func)
	local session = c.intcommand("TIMEOUT",ti)
	assert(session)
	local co = co_create(func)
	assert(session_id_coroutine[session] == nil)

	-- 记录当前 session 对应的 coroutine
	session_id_coroutine[session] = co
end

-- 将当前 coroutine 挂起 ti 个单位时间。一个单位是 1/100 秒。
-- 它是向框架注册一个定时器实现的。框架会在 ti 时间后，发送一个定时器消息来唤醒这个 coroutine 。
-- 这是一个阻塞 API 。它的返回值会告诉你是时间到了，还是被 skynet.wakeup 唤醒 （返回 "BREAK"）。
function skynet.sleep(ti)
	local session = c.intcommand("TIMEOUT",ti)
	assert(session)
	local succ, ret = coroutine_yield("SLEEP", session)
	sleep_session[coroutine.running()] = nil
	if succ then
		return
	end

	if ret == "BREAK" then
		return "BREAK"
	else
		error(ret)
	end
end

-- 相当于 skynet.sleep(0) 。交出当前服务对 CPU 的控制权。
-- 通常在你想做大量的操作，又没有机会调用阻塞 API 时，可以选择调用 yield 让系统跑的更平滑。
function skynet.yield()
	return skynet.sleep(0)
end

-- 把当前 coroutine 挂起。通常这个函数需要结合 skynet.wakeup 使用。
function skynet.wait()
	local session = c.genid()	-- 生成新的 session
	local ret, msg = coroutine_yield("SLEEP", session)
	local co = coroutine.running()
	sleep_session[co] = nil
	session_id_coroutine[session] = nil
end

-- 用于获得服务自己的地址。
local self_handle
function skynet.self()
	if self_handle then
		return self_handle
	end
	self_handle = string_to_handle(c.command("REG"))
	return self_handle
end

-- 用来查询一个 . 开头的名字对应的地址。它是一个非阻塞 API ，不可以查询跨节点的全局名字。
function skynet.localname(name)
	local addr = c.command("QUERY", name)
	if addr then
		return string_to_handle(addr)
	end
end

-- 将返回 skynet 节点进程启动的时间。这个返回值的数值本身意义不大，不同节点在同一时刻取到的值也不相同。
-- 只有两次调用的差值才有意义。用来测量经过的时间。每 100 表示真实时间 1 秒。
-- 这个函数的开销小于查询系统时钟。在同一个时间片内（没有因为阻塞 API 挂起），这个值是不变的。
function skynet.now()
	return c.intcommand("NOW")
end

-- 返回 skynet 节点进程启动的 UTC 时间，以秒为单位。
function skynet.starttime()
	return c.intcommand("STARTTIME")
end

-- 返回以秒为单位（精度为小数点后两位）的 UTC 时间。
function skynet.time()
	return skynet.now()/100 + skynet.starttime()	-- get now first would be better
end

-- 用于退出当前的服务。skynet.exit 之后的代码都不会被运行。而且，当前服务被阻塞住的 coroutine 也会立刻中断退出。
-- 这些通常是一些 RPC 尚未收到回应。所以调用 skynet.exit() 请务必小心。
-- 关闭了当前的服务, 所以对于之前请求的消息, 将以 PTYPE_ERROR 的类型返回给消息源.
function skynet.exit()
	fork_queue = {}	-- no fork coroutine can be execute after skynet.exit, 没有被 fork 的协程能够在 skynet.exit 后执行
	skynet.send(".launcher","lua","REMOVE",skynet.self(), false)

	-- report the sources that call me
	-- 以 PTYPE_ERROR 类型给消息源发送消息.
	for co, session in pairs(session_coroutine_id) do
		local address = session_coroutine_address[co]
		if session ~= 0 and address then
			c.redirect(address, 0, skynet.PTYPE_ERROR, session, "")
		end
	end

	for resp in pairs(unresponse) do
		resp(false)
	end

	-- report the sources I call but haven't return
	-- 给 watching_session 里面消息的消息源发送 PTYPE_ERROR 类型消息
	local tmp = {}
	for session, address in pairs(watching_session) do
		tmp[address] = true
	end

	for address in pairs(tmp) do
		c.redirect(address, 0, skynet.PTYPE_ERROR, 0, "")
	end

	-- 释放掉 skynet_context 的资源
	c.command("EXIT")

	-- quit service
	-- 退出服务
	coroutine_yield "QUIT"
end

-- 得到当前节点的一些全局配置, 服务间通用
function skynet.getenv(key)
	local ret = c.command("GETENV",key)
	if ret == "" then
		return
	else
		return ret
	end
end

-- 设置当前节点的一些全局配置, 服务间通用
function skynet.setenv(key, value)
	c.command("SETENV",key .. " " ..value)
end

-- 这条 API 可以把一条类别为 typename 的消息发送给 address 。它会先经过事先注册的 pack 函数打包 ... 的内容。
-- 是一条非阻塞 API ，发送完消息后，coroutine 会继续向下运行，这期间服务不会重入。
function skynet.send(addr, typename, ...)
	local p = proto[typename]
	return c.send(addr, p.id, 0, p.pack(...))
end

-- 生成一个唯一 session 号。
skynet.genid = assert(c.genid)

-- 它和 skynet.send 功能类似，但更细节一些。它可以指定发送地址（把消息源伪装成另一个服务），
-- 指定发送的消息的 session 。注：address 和 source 都必须是数字地址，不可以是别名。
skynet.redirect = function(dest,source,typename,...)
	return c.redirect(dest, source, proto[typename].id, ...)
end

skynet.pack = assert(c.pack)
skynet.packstring = assert(c.packstring)
skynet.unpack = assert(c.unpack)
skynet.tostring = assert(c.tostring)
skynet.trash = assert(c.trash)

-- 挂起当前的协程, 记录 session
local function yield_call(service, session)
	watching_session[session] = service
	local succ, msg, sz = coroutine_yield("CALL", session)		-- session_id_coroutine[param] = co, session -> coroutine
	watching_session[session] = nil
	if not succ then
		error "call failed"
	end
	return msg,sz
end

-- 这条 API 则不同，它会在内部生成一个唯一 session ，并向 address 提起请求，并阻塞等待对 session 的回应（可以不由 address 回应）。
-- 当消息回应后，还会通过之前注册的 unpack 函数解包。表面上看起来，就是发起了一次 RPC ，并阻塞等待回应。
function skynet.call(addr, typename, ...)
	local p = proto[typename]
	local session = c.send(addr, p.id , nil , p.pack(...))
	if session == nil then
		error("call to invalid address " .. skynet.address(addr))
	end
	return p.unpack(yield_call(addr, session))
end

-- 它和 skynet.call 功能类似（也是阻塞 API）。但发送时不经过 pack 打包流程，收到回应后，也不走 unpack 流程。
function skynet.rawcall(addr, typename, msg, sz)
	local p = proto[typename]
	local session = assert(c.send(addr, p.id , nil , msg, sz), "call to invalid address")
	return yield_call(addr, session)
end

-- 它会将 message size 对应的消息附上当前消息的 session ，以及 skynet.PTYPE_RESPONSE 这个类别，发送给当前消息的来源 source 。
function skynet.ret(msg, sz)
	msg = msg or ""
	return coroutine_yield("RETURN", msg, sz)
end

-- 返回的闭包可用于延迟回应。调用它(返回的函数)时，第一个参数通常是 true 表示是一个正常的回应，之后的参数是需要回应的数据。
-- 如果是 false ，则给请求者抛出一个异常。它的返回值表示回应的地址是否还有效。
-- 如果你仅仅想知道回应地址的有效性，那么可以在第一个参数传入 "TEST" 用于检测。
function skynet.response(pack)
	pack = pack or skynet.pack
	return coroutine_yield("RESPONSE", pack)
end

-- 与 skynet.ret 功能相同, 区别的地方在于, 此函数对传入的参数调用 skynet.pack 方法
function skynet.retpack(...)
	return skynet.ret(skynet.pack(...))
end

-- 唤醒一个被 skynet.sleep 或 skynet.wait 挂起的 coroutine
function skynet.wakeup(co)
	-- 实现细节参考 dispatch_wakeup, 这里只是做 1 个标记
	if sleep_session[co] and wakeup_session[co] == nil then
		wakeup_session[co] = true
		return true
	end
end

-- 注册特定类消息的处理函数。
function skynet.dispatch(typename, func)
	local p = proto[typename]
	if func then
		local ret = p.dispatch
		p.dispatch = func
		return ret
	else
		return p and p.dispatch
	end
end

-- 打印未知的请求错误, 并抛出错误
local function unknown_request(session, address, msg, sz, prototype)
	skynet.error(string.format("Unknown request (%s): %s", prototype, c.tostring(msg,sz)))
	error(string.format("Unknown session : %d from %x", session, address))
end

-- 设置新的 unknown_request, 返回设置前的 dispatch_unknown_request
function skynet.dispatch_unknown_request(unknown)
	local prev = unknown_request
	unknown_request = unknown
	return prev
end

-- 打印未知的响应错误, 并抛出错误
local function unknown_response(session, address, msg, sz)
	skynet.error(string.format("Response message : %s" , c.tostring(msg,sz)))
	error(string.format("Unknown session : %d from %x", session, address))
end

-- 设置新的 unknown_response, 返回设置前的 unknown_response
function skynet.dispatch_unknown_response(unknown)
	local prev = unknown_response
	unknown_response = unknown
	return prev
end

local tunpack = table.unpack

-- 从功能上，它等价于 skynet.timeout(0, function() func(...) end) 但是比 timeout 高效一点。因为它并不需要向框架注册一个定时器。
function skynet.fork(func,...)
	local args = { ... }
	local co = co_create(function()
		func(tunpack(args))
	end)
	table.insert(fork_queue, co)
	return co
end

-- 
local function raw_dispatch_message(prototype, msg, sz, session, source, ...)
	-- skynet.PTYPE_RESPONSE = 1, read skynet.h
	if prototype == 1 then	-- 处理响应类型的消息
		local co = session_id_coroutine[session]
		if co == "BREAK" then
			session_id_coroutine[session] = nil
		elseif co == nil then
			unknown_response(session, source, msg, sz)
		else
			session_id_coroutine[session] = nil
			suspend(co, coroutine.resume(co, true, msg, sz))
		end
	else
		local p = proto[prototype]

		-- 确认注册了该类型
		if p == nil then
			if session ~= 0 then	-- 如果需要回应, 则告诉请求服务发生错误
				c.send(source, skynet.PTYPE_ERROR, session, "")
			else	-- 报告未知的请求类型
				unknown_request(session, source, msg, sz, prototype)
			end
			return
		end

		local f = p.dispatch
		if f then
			local ref = watching_service[source]
			if ref then
				watching_service[source] = ref + 1
			else
				watching_service[source] = 1
			end

			local co = co_create(f)
			session_coroutine_id[co] = session
			session_coroutine_address[co] = source
			suspend(co, coroutine.resume(co, session, source, p.unpack(msg, sz, ...)))
		else
			unknown_request(session, source, msg, sz, proto[prototype].name)
		end
	end
end

function skynet.dispatch_message(...)
	local succ, err = pcall(raw_dispatch_message,...)
	while true do
		local key,co = next(fork_queue)
		if co == nil then
			break
		end

		fork_queue[key] = nil
		local fork_succ, fork_err = pcall(suspend,co,coroutine.resume(co))
		if not fork_succ then
			if succ then
				succ = false
				err = tostring(fork_err)
			else
				err = tostring(err) .. "\n" .. tostring(fork_err)
			end
		end
	end
	assert(succ, tostring(err))
end

-- 用于启动一个新的 Lua 服务。name 是脚本的名字（不用写 .lua 后缀）。
-- 只有被启动的脚本的 start 函数返回后，这个 API 才会返回启动的服务的地址，这是一个阻塞 API 。
-- 如果被启动的脚本在初始化环节抛出异常，或在初始化完成前就调用 skynet.exit 退出，｀skynet.newservice` 都会抛出异常。
-- 如果被启动的脚本的 start 函数是一个永不结束的循环，那么 newservice 也会被永远阻塞住。
function skynet.newservice(name, ...)
	return skynet.call(".launcher", "lua" , "LAUNCH", "snlua", name, ...)
end

function skynet.uniqueservice(global, ...)
	if global == true then
		return assert(skynet.call(".service", "lua", "GLAUNCH", ...))
	else
		return assert(skynet.call(".service", "lua", "LAUNCH", global, ...))
	end
end

function skynet.queryservice(global, ...)
	if global == true then
		return assert(skynet.call(".service", "lua", "GQUERY", ...))
	else
		return assert(skynet.call(".service", "lua", "QUERY", global, ...))
	end
end

function skynet.address(addr)
	if type(addr) == "number" then
		return string.format(":%08x",addr)
	else
		return tostring(addr)
	end
end

function skynet.harbor(addr)
	return c.harbor(addr)
end

function skynet.error(...)
	local t = {...}
	for i=1,#t do
		t[i] = tostring(t[i])
	end
	return c.error(table.concat(t, " "))
end

----- register protocol
do
	local REG = skynet.register_protocol

	REG {
		name = "lua",
		id = skynet.PTYPE_LUA,
		pack = skynet.pack,
		unpack = skynet.unpack,
	}

	REG {
		name = "response",
		id = skynet.PTYPE_RESPONSE,
	}

	REG {
		name = "error",
		id = skynet.PTYPE_ERROR,
		unpack = function(...) return ... end,
		dispatch = _error_dispatch,
	}
end

local init_func = {}

function skynet.init(f, name)
	assert(type(f) == "function")
	if init_func == nil then
		f()
	else
		if name == nil then
			table.insert(init_func, f)
		else
			assert(init_func[name] == nil)
			init_func[name] = f
		end
	end
end

local function init_all()
	local funcs = init_func
	init_func = nil
	if funcs then
		for k,v in pairs(funcs) do
			v()
		end
	end
end

local function init_template(start)
	init_all()
	init_func = {}
	start()
	init_all()
end

function skynet.pcall(start)
	return xpcall(init_template, debug.traceback, start)
end

function skynet.init_service(start)
	local ok, err = skynet.pcall(start)
	if not ok then
		skynet.error("init service failed: " .. tostring(err))
		skynet.send(".launcher","lua", "ERROR")
		skynet.exit()
	else
		skynet.send(".launcher","lua", "LAUNCHOK")
	end
end

function skynet.start(start_func)
	c.callback(skynet.dispatch_message)
	skynet.timeout(0, function()
		skynet.init_service(start_func)
	end)
end

function skynet.endless()
	return c.command("ENDLESS")~=nil
end

function skynet.mqlen()
	return c.intcommand "MQLEN"
end

function skynet.task(ret)
	local t = 0
	for session,co in pairs(session_id_coroutine) do
		if ret then
			ret[session] = debug.traceback(co)
		end
		t = t + 1
	end
	return t
end

function skynet.term(service)
	return _error_dispatch(0, service)
end

local function clear_pool()
	coroutine_pool = {}
end

-- Inject internal debug framework
local debug = require "skynet.debug"
debug(skynet, {
	dispatch = skynet.dispatch_message,
	clear = clear_pool,
	suspend = suspend,
})

return skynet
