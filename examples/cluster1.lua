local skynet = require "skynet"
local cluster = require "cluster"
local snax = require "snax"

skynet.start(function()
<<<<<<< HEAD
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
=======
	local sdb = skynet.newservice("simpledb")
	skynet.name(".simpledb", sdb)
	print(skynet.call(".simpledb", "lua", "SET", "a", "foobar"))
	print(skynet.call(".simpledb", "lua", "SET", "b", "foobar2"))
	print(skynet.call(".simpledb", "lua", "GET", "a"))
	print(skynet.call(".simpledb", "lua", "GET", "b"))
	cluster.open "db"
	cluster.open "db2"
	-- unique snax service
	snax.uniqueservice "pingserver"
end)
>>>>>>> parent of 84d5ec2... Merge branch 'cloudwu/master'
