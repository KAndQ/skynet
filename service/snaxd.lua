-- 通过 skynet_context 启动 1 个 snax 服务.

local skynet = require "skynet"
local c = require "skynet.core"
local snax_interface = require "snax.interface"
local profile = require "profile"
local snax = require "snax"

-- 获得传入的 snax 服务名
local snax_name = tostring(...)

-- 获得服务的处理函数集合, 匹配模式
local func, pattern = snax_interface(snax_name, _ENV)

-- 将 snax_name 的路径添加到 lua 搜索路径中
local snax_path = pattern:sub(1, pattern:find("?", 1, true) - 1) .. snax_name ..  "/"
package.path = snax_path .. "?.lua;" .. package.path

SERVICE_NAME = snax_name
SERVICE_PATH = snax_path

local profile_table = {}

-- 状态更新
-- @param name 服务名
-- @param ti 间隔时间
local function update_stat(name, ti)
	local t = profile_table[name]
	if t == nil then
		t = { count = 0,  time = 0 }
		profile_table[name] = t
	end
	t.count = t.count + 1	-- accept, response 使用次数
	t.time = t.time + ti	-- 累计时间
end

local traceback = debug.traceback

-- 目前没有使用
local function do_func(f, msg)
	return xpcall(f, traceback, table.unpack(msg))
end

-- 将 f 函数的返回结果进行 skynet.pack, 目前没有使用
-- @param f 执行函数
-- @param ... f 执行参数传入的参数
-- @return msg, sz
local function dispatch(f, ...)
	return skynet.pack(f(...))
end

-- 执行 f 函数, 并且将返回值响应给请求服务
-- @param f 执行函数
-- @param ... 执行 f 函数的参数
-- @return 响应成功返回 true
local function return_f(f, ...)
	return skynet.ret(skynet.pack(f(...)))
end

-- 执行函数, 并且对执行时间做统计
-- @param method func 中的元素
-- @param ... 函数执行需要的参数
local function timing(method, ...)
	local err, msg
	profile.start()
	if method[2] == "accept" then
		-- no return
		err,msg = xpcall(method[4], traceback, ...)
	else
		err,msg = xpcall(return_f, traceback, method[4], ...)
	end
	local ti = profile.stop()
	update_stat(method[3], ti)
	assert(err,msg)
end

skynet.start(function()
	local init = false

	local function dispatcher(session, source , id, ...)
		local method = func[id]

		if method[2] == "system" then
			local command = method[3]

			if command == "hotfix" then
				local hotfix = require "snax.hotfix"
				skynet.ret(skynet.pack(hotfix(func, ...)))
			elseif command == "init" then
				assert(not init, "Already init")
				local initfunc = method[4] or function() end
				initfunc(...)
				skynet.ret()
				skynet.info_func(function()
					return profile_table
				end)
				init = true
			else
				assert(init, "Never init")
				assert(command == "exit")
				local exitfunc = method[4] or function() end
				exitfunc(...)
				skynet.ret()
				init = false
				skynet.exit()
			end
		else	-- 执行 accept, response 组的函数
			assert(init, "Init first")
			timing(method, ...)
		end
	end

	skynet.dispatch("snax", dispatcher)

	-- set lua dispatcher
	function snax.enablecluster()
		skynet.dispatch("lua", dispatcher)
	end
end)
