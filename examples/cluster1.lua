local skynet = require "skynet"
local cluster = require "cluster"
local snax = require "snax"

skynet.start(function()
<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> parent of 5702862... Merge branch 'cloudwu/master'
    local sdb = skynet.newservice("simpledb")
    -- register name "sdb" for simpledb, you can use cluster.query() later.
    -- See cluster2.lua
    cluster.register("sdb", sdb)
<<<<<<< HEAD

    print(skynet.call(sdb, "lua", "SET", "a", "foobar"))
    print(skynet.call(sdb, "lua", "SET", "b", "foobar2"))
    print(skynet.call(sdb, "lua", "GET", "a"))
    print(skynet.call(sdb, "lua", "GET", "b"))
    cluster.open "db"
    cluster.open "db2"
    -- unique snax service
    snax.uniqueservice "pingserver"
end)
=======
	local sdb = skynet.newservice("simpledb")
	-- register name "sdb" for simpledb, you can use cluster.query() later.
	-- See cluster2.lua
	cluster.register("sdb", sdb)

	print(skynet.call(sdb, "lua", "SET", "a", "foobar"))
	print(skynet.call(sdb, "lua", "SET", "b", "foobar2"))
	print(skynet.call(sdb, "lua", "GET", "a"))
	print(skynet.call(sdb, "lua", "GET", "b"))
	cluster.open "db"
	cluster.open "db2"
	-- unique snax service
	snax.uniqueservice "pingserver"
end)
>>>>>>> cloudwu/master
=======

    print(skynet.call(sdb, "lua", "SET", "a", "foobar"))
    print(skynet.call(sdb, "lua", "SET", "b", "foobar2"))
    print(skynet.call(sdb, "lua", "GET", "a"))
    print(skynet.call(sdb, "lua", "GET", "b"))
    cluster.open "db"
    cluster.open "db2"
    -- unique snax service
    snax.uniqueservice "pingserver"
end)
>>>>>>> parent of 5702862... Merge branch 'cloudwu/master'
