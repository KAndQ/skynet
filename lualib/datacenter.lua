-- 注意：这三个 api 都会阻塞住当前 coroutine ，留心异步重入的问题。

local skynet = require "skynet"

local datacenter = {}

-- datacenter.get(key1, key2, ...) 从 key1.key2 读一个值。
-- 这个 api 至少需要一个参数，如果传入多个参数，则用来读出树的一个分支。
function datacenter.get(...)
	return skynet.call("DATACENTER", "lua", "QUERY", ...)
end

-- datacenter.set(key1, key2, ... , value) 可以向 key1.key2 设置一个值 value 。
-- 这个 api 至少需要两个参数，没有特别限制树结构的层级数
function datacenter.set(...)
	return skynet.call("DATACENTER", "lua", "UPDATE", ...)
end

-- 同 get 方法，但如果读取的分支为 nil 时，这个函数会阻塞，直到有人更新这个分支才返回。
-- 当读写次序不确定，但你需要读到其它地方写入的数据后再做后续事情时，用它比循环尝试读取要好的多。
-- wait 必须作用于一个叶节点，不能等待一个分支。
function datacenter.wait(...)
	return skynet.call("DATACENTER", "lua", "WAIT", ...)
end

return datacenter

