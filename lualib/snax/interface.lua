local skynet = require "skynet"

-- 加载 snax 服务, 将 snax 服务的 accept 和 response 表内的函数和如果定义了 init, exit, hotfix 函数的话, 
-- 全部添加到 func 集合中返回.
-- @param name snax 服务名
-- @param G table 类型, 设定 snax 的环境
-- @param loader 加载 snax 服务的加载器
-- @return 返回两个参数: snax 服务定义的函数集合和匹配的模式
return function (name, G, loader)
	loader = loader or loadfile	-- 可以联系 snax.hotfix.lua 源码, 理解更深刻
	local mainfunc

	-- 返回 1 个表, 该表在更新的时候只接收函数作为值
	-- @param id table 类型, 返回的表在每次更新的时候都会向 id 里面插入更新的数据
	-- @param group 组分类, 目前是 "accept", "response", "system"
	-- @return table
	local function func_id(id, group)
		local tmp = {}

		-- 在返回表被更新的时候, 执行的元方法
		local function count(_, name, func)
			-- 名字检查
			if type(name) ~= "string" then
				error (string.format("%s method only support string", group))
			end

			-- func 参数检查
			if type(func) ~= "function" then
				error (string.format("%s.%s must be function"), group, name)
			end

			-- 重复定义检查
			if tmp[name] then
				error (string.format("%s.%s duplicate definition", group, name))
			end

			tmp[name] = true
			table.insert(id, { #id + 1, group, name, func} )
		end

		return setmetatable({}, { __newindex = count })
	end

	-- G 检查
	do
		assert(getmetatable(G) == nil)
		assert(G.init == nil)
		assert(G.exit == nil)

		assert(G.accept == nil)
		assert(G.response == nil)
	end

	local temp_global = {}
	local env = setmetatable({} , { __index = temp_global })
	local func = {}

	--[[
		system[1] = "init"
		system[2] = "exit"
		system[3] = "hotfix"
		system["init"] = 1
		system["exit"] = 2
		system["hotfix"] = 3
		----
		func[1] = { 1, "system", "init" }
		func[2] = { 2, "system", "exit" }
		func[3] = { 3, "system", "hotfix" }
	--]]
	local system = { "init", "exit", "hotfix" }
	do
		for k, v in ipairs(system) do
			system[v] = k
			func[k] = { k , "system", v }
		end
	end

	-- 注意, 每次更新 accept 和 response 表都会向 func 表中插入数据, 该数据值只能是函数
	env.accept = func_id(func, "accept")
	env.response = func_id(func, "response")

	-- 当对 G 表做修改时调用的元方法
	local function init_system(t, name, f)
		local index = system[name]	-- "init", "exit", "hotfix"

		-- 如果是原先不存在于 system 中表的元素, 就将 name 和 f 存入到 temp_global 中.
		-- 否则直接更新 func 内元素的, 函数元素(4)
		if index then
			if type(f) ~= "function" then
				error (string.format("%s must be a function", name))
			end
			func[index][4] = f
		else
			temp_global[name] = f
		end
	end

	-- 成功加载 snax 服务文件的模式
	local pattern

	do
		-- 拿到 snax 服务的查询路径
		local path = assert(skynet.getenv "snax" , "please set snax in config file")

		local errlist = {}

		-- 从配置路径中搜索到 snax 服务文件
		for pat in string.gmatch(path,"[^;]+") do
			local filename = string.gsub(pat, "?", name)
			local f , err = loader(filename, "bt", G)	-- G 作为接下来加载模块的环境
			if f then
				pattern = pat
				mainfunc = f
				break
			else
				table.insert(errlist, err)
			end
		end

		-- 如果没有成功加载 snax 服务, 抛出错误
		if mainfunc == nil then
			error(table.concat(errlist, "\n"))
		end
	end
	
	-- 删除 G 的元表
	setmetatable(G,	{ __index = env , __newindex = init_system })

	-- 执行 snax 服务
	local ok, err = pcall(mainfunc)
	setmetatable(G, nil)
	assert(ok,err)

	-- temp_global 的数据复制给 G, 因为之前 G 无论是获得还是更新, 都是使用元表方法. 无法操作自身这个表
	for k, v in pairs(temp_global) do
		G[k] = v
	end

	return func, pattern
end
