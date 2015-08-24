-- 这个文件用于 config 中的 lualoader 配置项, 由这段代码解析服务名称，进一步加载 lua 代码。
-- snlua 会将下面几个配置项取出:
-- SERVICE_NAME 第一个参数，通常是服务名。
-- LUA_PATH config 文件中配置的 lua_path。
-- LUA_CPATH config 文件中配置的 lua_cpath。
-- LUA_PRELOAD config 文件中配置的 preload。
-- LUA_SERVICE config 文件中配置的 luaservice。

-- 参数以空格分割
local args = {}
for word in string.gmatch(..., "%S+") do
	table.insert(args, word)
end

-- 第一个参数是服务名
SERVICE_NAME = args[1]

local main, pattern

-- 从 config 的 luaservice 的配置中搜索出指定的文件
local err = {}

-- 模式解释: 匹配连续的 ";" 的补集, 并且以 0或多次 ";" 结束的字符串, 最后捕获连续的 ";" 的补集内容
-- 例如: ./service/?.lua;;;test/?.lua;examples/?.lua, 得到的是 ./service/?.lua test/?.lua examples/?.lua
for pat in string.gmatch(LUA_SERVICE, "([^;]+);*") do
	local filename = string.gsub(pat, "?", SERVICE_NAME)	-- 替换使用 SERVICE_NAME 替换 "?"
	local f, msg = loadfile(filename)
	if not f then
		table.insert(err, msg)
	else
		pattern = pat
		main = f
		break
	end
end

if not main then
	error(table.concat(err, "\n"))
end

LUA_SERVICE = nil
package.path , LUA_PATH = LUA_PATH
package.cpath , LUA_CPATH = LUA_CPATH

-- 模式解释: 匹配以 ./ 和 [/? 的补集] 为结束的字符串, 并且捕获包括 ./ 和 ./ 之前内容的字符串, 这里的 . 表示的是所有字符
-- 例如: 1111./2222 得到的是 1111./; ./service 得到的是 ./; ./service/?.lua 得到的是 nil;
local service_path = string.match(pattern, "(.*/)[^/?]+$")

if service_path then	-- 如果 service_path 是路径, 则添加到 package.path 中
	service_path = string.gsub(service_path, "?", args[1])
	package.path = service_path .. "?.lua;" .. package.path
	SERVICE_PATH = service_path
else	-- 表示的如果是文件, 得到文件所在的文件夹路径
	local p = string.match(pattern, "(.*/).+$")
	SERVICE_PATH = p
end

-- 加载 lua 服务代码前，loader 会尝试先运行一个 preload 制定的脚本。
if LUA_PRELOAD then
	local f = assert(loadfile(LUA_PRELOAD))
	f(table.unpack(args))
	LUA_PRELOAD = nil
end

-- 执行加载的脚本
-- #issue 这里为什么是接收第 2 个参数之后的部分有些不理解, 从 snlua 来看, 只传入了 1 个参数, 所以这里传入的参数总为 nil.
main(select(2, table.unpack(args)))
