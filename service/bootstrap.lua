local skynet = require "skynet"
local harbor = require "skynet.harbor"
require "skynet.manager"	-- import skynet.launch, ...

skynet.start(function()
	local standalone = skynet.getenv "standalone"

	-- 开启 launcher.lua 服务
	local launcher = assert(skynet.launch("snlua","launcher"))
	skynet.name(".launcher", launcher)

	local harbor_id = tonumber(skynet.getenv "harbor")
	if harbor_id == 0 then	-- harbor 为 0, 表示就是启动 1 个单节点模式
		-- 单节点下, standalone 
		assert(standalone == nil)
		standalone = true
		skynet.setenv("standalone", "true")

		-- 单节点模式下，是不需要通过内置的 harbor 机制做节点中通讯的。
		-- 但为了兼容（因为你还是有可能注册全局名字），需要启动一个叫做 cdummy 的服务，它负责拦截对外广播的全局名字变更。
		local ok, slave = pcall(skynet.newservice, "cdummy")
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)
	else
		-- 如果是多节点模式，对于 master 节点，需要启动 cmaster 服务作节点调度用。
		-- 对于 master 节点必须要配置 standalone 参数.
		if standalone then
			if not pcall(skynet.newservice,"cmaster") then
				skynet.abort()
			end
		end

		-- 此外，每个节点（包括 master 节点自己）都需要启动 cslave 服务，用于节点间的消息转发，以及同步全局名字。
		local ok, slave = pcall(skynet.newservice, "cslave")
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)
	end

	-- 如果当前包含 master, 则启动 DataCenter 服务
	-- datacenter 可用来在整个 skynet 网络做跨节点的数据共享。
	if standalone then
		local datacenter = skynet.newservice "datacenterd"
		skynet.name("DATACENTER", datacenter)
	end

	-- 启动用于 UniqueService 管理的 service_mgr
	skynet.newservice "service_mgr"

	-- 启动用户定义的服务
	pcall(skynet.newservice, skynet.getenv "start" or "main")

	skynet.exit()	-- 这个服务会在做完以上初始化之后, 马上退出
end)
