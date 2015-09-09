local skynet = require "skynet"
local datacenter = require "datacenter"

local function f1()
	print("====1==== wait hello")
	print("\t1>",datacenter.wait ("hello"))	-- 获得 world, 因为已经插入了 hello
	print("====1==== wait key.foobar")
	print("\t1>", pcall(datacenter.wait,"key"))	-- will failed, because "key" is a branch, 将失败, 因为 "key" 是 1 个分支
	print("\t1>",datacenter.wait ("key", "foobar"))	-- 获得 bingo
end

local function f2()
	skynet.sleep(10)	-- 睡眠, 为的是先让 f1 执行
	print("====2==== set key.foobar")
	datacenter.set("key", "foobar", "bingo")	-- 更新 key: key.foobar, value = bingo
end

skynet.start(function()
	datacenter.set("hello", "world")	-- 更新 key = hello, value = world
	print(datacenter.get "hello")		-- 获得 world
	skynet.fork(f1)
	skynet.fork(f2)
end)
