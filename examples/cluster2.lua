local skynet = require "skynet"
local cluster = require "cluster"

skynet.start(function()
<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
    -- query name "sdb" of cluster db.
    local sdb = cluster.query("db", "sdb")
    print("db.sbd=",sdb)
    local proxy = cluster.proxy("db", sdb)
    local largekey = string.rep("X", 128*1024)
    local largevalue = string.rep("R", 100 * 1024)
    print(skynet.call(proxy, "lua", "SET", largekey, largevalue))
    local v = skynet.call(proxy, "lua", "GET", largekey)
    assert(largevalue == v)
=======
=======
>>>>>>> cloudwu/master
	-- query name "sdb" of cluster db.
	local sdb = cluster.query("db", "sdb")
	print("db.sbd=",sdb)
	local proxy = cluster.proxy("db", sdb)
=======
	local proxy = cluster.proxy("db", ".simpledb")
>>>>>>> parent of c2aa2e4... merge 'cloudwu/skynet'
	local largekey = string.rep("X", 128*1024)
	local largevalue = string.rep("R", 100 * 1024)
	print(skynet.call(proxy, "lua", "SET", largekey, largevalue))
	local v = skynet.call(proxy, "lua", "GET", largekey)
	assert(largevalue == v)
<<<<<<< HEAD

	print(cluster.call("db", sdb, "GET", "a"))
	print(cluster.call("db2", sdb, "GET", "b"))
<<<<<<< HEAD
>>>>>>> cloudwu/master
=======
>>>>>>> parent of c2aa2e4... merge 'cloudwu/skynet'

	print(cluster.call("db", ".simpledb", "GET", "a"))
	print(cluster.call("db2", ".simpledb", "GET", "b"))

<<<<<<< HEAD
    -- test snax service
    local pingserver = cluster.snax("db", "pingserver")
    print(pingserver.req.ping "hello")
end)
=======

=======
>>>>>>> parent of c2aa2e4... merge 'cloudwu/skynet'
	-- test snax service
	local pingserver = cluster.snax("db", "pingserver")
	print(pingserver.req.ping "hello")
end)
<<<<<<< HEAD
>>>>>>> cloudwu/master
=======
>>>>>>> parent of c2aa2e4... merge 'cloudwu/skynet'
