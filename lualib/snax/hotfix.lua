-- 关于 lua _ENV 的介绍, 在当前文件中, 操作 _ENV 比较多.
-- function 在 lua 中称为 closure ，仅仅只是函数体和 upvalue 的联合体。
-- http://tieba.baidu.com/p/2208184626

-- snax 是支持热更新的（只能热更新 snax 框架编写的 lua 服务）。
-- 但热更新更多的用途是做不停机的 bug 修复，不应用于常规的版本更新。
-- 所以，热更新的 api 被设计成下面这个样子。更适合打补丁。
-- 注：不可以在 patch 中增加新的远程方法。

-- 你可以通过 snax.hotfix(obj, patchcode) 来向 obj 提交一个 patch 。
-- 举个例子，你可以向上面列出的 pingserver 提交一个 patch ：
--[===[
snax.hotfix(obj, [[

	local i
	local hello

	function accept.hello()
	    i = i + 1
	    print ("fix", i, hello)
	end

	function hotfix(...)
	    local temp = i
	    i = 100
	    return temp
	end

]]))
--]===]

-- 这样便改写了 accept.hello 方法。在 patch 中声明的 local i 和 local hello 在之前的版本中也有同名的 local 变量。 
-- snax 的热更新机制会重新映射这些 local 变量。让 patch 中的新函数对应已有的 local 变量，所以你可以安全的继承服务的内部状态。

-- patch 中可以包含一个 function hotfix(...) 函数，在 patch 提交后立刻执行。
-- 这个函数可以用来查看或修改 snax 服务的线上状态（因为 local 变量会被关联）。
-- hotfix 的函数返回值会传递给 snax.hotfix 的调用者。

-- 所以，你也可以提交一个仅包含 hotfix 函数的 patch ，而不修改任何代码。
-- 这样的 patch 通常用于查看 snax 服务的内部状态（内部 local 变量的值），或用于修改它们。

local si = require "snax.interface"
local io = io

local hotfix = {}

-- 获得 f 函数 _ENV 这个 upvalue 的唯一标识符
-- @param f 函数
-- @return _ENV 的唯一标识符
local function envid(f)
	local i = 1
	while true do
		-- debug.getupvalue (f, up)
		-- 此函数返回函数 f 的第 up 个上值的名字和值。 如果该函数没有那个上值，返回 nil 。
		-- 以 '(' （开括号）打头的变量名表示没有名字的变量 （去除了调试信息的代码块）。
		local name, value = debug.getupvalue(f, i)
		if name == nil then
			return
		end

		if name == "_ENV" then
			-- debug.upvalueid (f, n)
			-- 返回指定函数第 n 个上值的唯一标识符（一个轻量用户数据）。
			-- 这个唯一标识符可以让程序检查两个不同的闭包是否共享了上值。 若 Lua 闭包之间共享的是同一个上值 （即指向一个外部局部变量），会返回相同的标识符。
			return debug.upvalueid(f, i)
		end

		i = i + 1
	end
end

-- 收集 f 的 upvalue
-- @param f 待收集的函数
-- @param uv 存储从 f 收集的 upvalue
-- @param env _ENV 的唯一 id
local function collect_uv(f, uv, env)
	local i = 1
	while true do
		-- 得到 upvalue 的名字和值
		local name, value = debug.getupvalue(f, i)
		if name == nil then
			break
		end

		-- 得到 upvalue 的唯一标识符
		local id = debug.upvalueid(f, i)

		if uv[name] then	-- 保证函数共享使用的 upvalue(_ENV) 相同
			assert(uv[name].id == id, string.format("ambiguity local value %s", name))
		else
			-- 存储收集到的 upvalue
			uv[name] = { func = f, index = i, id = id }

			-- 如果是 function 类型, 那么继续递归查询 upvalue
			if type(value) == "function" then
				if envid(value) == env then
					collect_uv(value, uv, env)
				end
			end
		end

		i = i + 1
	end
end

-- 收集 funcs 元素的函数 upvalue
-- @param funcs snax_interface 生成的集合
-- @return 收集的 upvalue 集合
local function collect_all_uv(funcs)
	local global = {}

	for _, v in pairs(funcs) do
		if v[4] then
			collect_uv(v[4], global, envid(v[4]))
		end
	end

	if not global["_ENV"] then
		global["_ENV"] = {func = collect_uv, index = 1}
	end

	return global
end

-- 返回编译好的 source 代码块函数
-- @param source 源代码字符串
-- @return 返回函数, 传入的 filename 参数不会使用, 直接加载 source 内容
local function loader(source)
	return function (filename, ...)
		return load(source, "=patch", ...)
	end
end

-- 从 funcs 里面搜索到符合 group, name 的元素
-- @param funcs 由 snax_interface 生成的集合
-- @param group "system", "accept", "response"
-- @param name 函数名
-- @return funcs 的元素
local function find_func(funcs, group, name)
	for _, desc in pairs(funcs) do
		local _, g, n = table.unpack(desc)
		if group == g and name == n then
			return desc
		end
	end
end

-- 虚拟环境表
local dummy_env = {}

-- 将 funcs 的 upvalue 替换 f 函数的 upvalue, 同时 f 将作为新的函数替换掉之前版本的函数
-- @param funcs snax_interface 生成的集合, 之前版本的服务
-- @param gorup "system", "accept", "response"
-- @param name 函数的名字
-- @param f 待更新的函数
local function patch_func(funcs, global, group, name, f)
	-- 确保 funcs(原来的 snax 服务) 中存在, 这里决定 source 中不能增加新的方法
	local desc = assert(find_func(funcs, group, name), string.format("Patch mismatch %s.%s", group, name))

	local i = 1
	while true do
		-- 找到 f 函数 upvalue 的名字和值
		local name, value = debug.getupvalue(f, i)

		if name == nil then
			break
		elseif value == nil or value == dummy_env then
			-- 将原来收集的相同名字的 upvalue 给 f 函数
			local old_uv = global[name]
			if old_uv then
				-- debug.upvaluejoin (f1, n1, f2, n2)
				-- 让 Lua 闭包 f1 的第 n1 个上值 引用 Lua 闭包 f2 的第 n2 个上值。
				debug.upvaluejoin(f, i, old_uv.func, old_uv.index)
			end
		end
		i = i + 1
	end

	-- 更新函数
	desc[4] = f
end

-- 注入热更新代码
-- @param funcs 由 snax_interface 生成的集合, 是原服务的集合
-- @param source snax 服务的热更新代码字符串
-- @param ... 执行热更新代码里面的 hotfix 函数需要的参数
-- @return 返回热更新的 hotfix 函数的返回值
local function inject(funcs, source, ...)
	-- 加载 source 内容
	local patch = si("patch", dummy_env, loader(source))

	-- 收集 funcs 的 upvalue
	local global = collect_all_uv(funcs)

	-- 将之前版本的 upvalue 全部更新到 f 中, 同时更新函数, f 作为新的处理函数
	for _, v in pairs(patch) do
		local _, group, name, f = table.unpack(v)
		if f then
			patch_func(funcs, global, group, name, f)
		end
	end

	-- 执行 source 的 hotfix 函数
	local hf = find_func(patch, "system", "hotfix")
	if hf and hf[4] then
		return hf[4](...)
	end
end

-- 注入 source 代码块
-- @param funcs 原 snax 服务的 snax_interface 返回集合
-- @param source 代码块
-- @param ... source 代码块的 hotfix 函数需要的参数
-- @return source 代码块 hotfix 函数的返回值
return function (funcs, source, ...)
	return pcall(inject, funcs, source, ...)
end
