-- 同一个 skynet 服务中的一条消息处理中，如果调用了一个阻塞 API ，那么它会被挂起。
-- 挂起过程中，这个服务可以响应其它消息。这很可能造成时序问题，要非常小心处理。

-- 换句话说，一旦你的消息处理过程有外部请求，那么先到的消息未必比后到的消息先处理完。
-- 且每个阻塞调用之后，服务的内部状态都未必和调用前的一致（因为别的消息处理过程可能改变状态）。

local skynet = require "skynet"
local coroutine = coroutine
local xpcall = xpcall
local traceback = debug.traceback
local table = table

-- 返回一个函数. 得到一个新的临界区。临界区可以保护一段代码不被同时运行。
function skynet.queue()
	local current_thread	-- 存储当前的协程
	local ref = 0			-- 对当前协程的重入的计数, 为 0 的时候才唤醒之前等待的协程
	local thread_queue = {}	-- 等待的协程队列

	return function(f, ...)
		local thread = coroutine.running()	-- 获得当前正在执行的协程

		-- 如果之前已经有其他的协程运行
		-- 那么将当前的协程放入到队列中, 并且阻塞(停止运行)
		if current_thread and current_thread ~= thread then
			table.insert(thread_queue, thread)	-- 存储当前的协程, 等待上一个协程唤醒
			skynet.wait()		-- 等待上一个协程唤醒
			assert(ref == 0)	-- current_thread == thread, 保证之前的协程已经执行结束
		end
		current_thread = thread

		ref = ref + 1	-- 如果 f 函数内又使用了当前函数, 那么计数 +1, 开始一次调用
		local ok, err = xpcall(f, traceback, ...)	-- 执行 f 函数, f 函数内部可能存在阻塞
		ref = ref - 1	-- 计数 -1 表示结束一次调用, 为 0 的时候才唤醒等待的协程
		if ref == 0 then
			-- 唤醒等待的协程
			current_thread = table.remove(thread_queue, 1)
			if current_thread then
				skynet.wakeup(current_thread)
			end
		end
		assert(ok, err)
	end
end

return skynet.queue
