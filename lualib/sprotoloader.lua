-- 原理是在 C 模块中提供了 16 个全局的 slot ，可以通过 sprotoloader.register 或 sprotoloader.save 在初始化时，加载需要的协议，并保存在这些 slot 里。
-- 通常我们只需要两个 slot ，一个用于保存客户端到服务器的协议组，另一个用于保存服务器到客户端的协议组。分别位于 slot 1 和 2 。
-- 注意：这套 api 并非线程安全。所以必须自行保证在初始化完毕后再做 load 操作。（load 本身是线程安全的）。

local parser = require "sprotoparser"
local core = require "sproto.core"
local sproto = require "sproto"

local loader = {}

-- 将文件内的内容解析为二进制数据存储起来
-- @param filename 文件名, 存储的 index, 以用来查询
function loader.register(filename, index)
	local f = assert(io.open(filename), "Can't open sproto file")
	local data = f:read "a"
	f:close()
	local sp = core.newproto(parser.parse(data))	-- cobj
	core.saveproto(sp, index)
end

-- 将 sprotoparser.parse 解析后的二进制数据存储起来
-- @param bin sprotoparser.parse 解析出来的数据
-- @param index 存储的 index, 以用来查询
function loader.save(bin, index)
	local sp = core.newproto(bin)	-- cobj
	core.saveproto(sp, index)
end

-- 在每个 vm 内，都可以通过 sprotoloader.load 把协议加载到 vm 中
-- @param index 使用之前存储的使用的 index
-- @return sproto 对象
function loader.load(index)
	local sp = core.loadproto(index)	-- cobj
	--  no __gc in metatable
	return sproto.sharenew(sp)
end

return loader

