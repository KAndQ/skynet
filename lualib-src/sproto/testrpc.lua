local sproto = require "sproto"
local print_r = require "print_r"

-- 服务器协议定义, 服务器 sproto 对象
local server_proto = sproto.parse [[
.package {
    type 0 : integer
    session 1 : integer
}

foobar 1 {
    request {
        what 0 : string
    }
    response {
        ok 0 : boolean
    }
}

foo 2 {
    response {
        ok 0 : boolean
    }
}

bar 3 {}

blackhole 4 {
}
]]

-- 客户端协议定义, 客户端 sproto 对象
local client_proto = sproto.parse [[
.package {
    type 0 : integer
    session 1 : integer
}
]]

print("=== default table")

print_r(server_proto:default("package"))    -- 打印 package 的默认 table 数据
print_r(server_proto:default("foobar", "REQUEST"))  -- 打印 foobar 的默认 table 数据

assert(server_proto:default("foo", "REQUEST") == nil)       -- 确认 foo 没有 request 协议
assert(server_proto:request_encode("foo") == "")            -- 确认 foo 没有 request 协议, 编码后的字符串对象值为空字符串
server_proto:response_encode("foo", { ok = true })          -- 将 foo 的 response 协议编码
assert(server_proto:request_decode("blackhole") == nil)     -- blackhole 请求协议解码, 但是 blackhole 是没有定义 request 的, 所以返回 nil
assert(server_proto:response_decode("blackhole") == nil)    -- blackhole 响应协议解码, 但是 blackhole 是没有定义 response 的, 所以返回 nil

print("=== test 1")

-- The type package must has two field : type and session
-- package 必须有含有 type 和 session 两个字段
local server = server_proto:host "package"          -- 生成服务器的 host 对象
local client = client_proto:host "package"          -- 生成客户端的 host 对象
local client_request = client:attach(server_proto)  -- 根据服务器协议, 生成客户端的请求函数

-- 客户端生成 foobar 的请求数据
print("client request foobar")
local req = client_request("foobar", { what = "foo" }, 1)
print("request foobar size =", #req)

-- 服务器处理 foobar 请求数据
local type, name, request, response = server:dispatch(req)
assert(type == "REQUEST" and name == "foobar")
print_r(request)

-- 服务器生成 foobar 响应数据
print("server response")
local resp = response { ok = true }
print("response package size =", #resp)

-- 客户端处理 foobar 响应数据
print("client dispatch")
local type, session, response = client:dispatch(resp)
assert(type == "RESPONSE" and session == 1)
print_r(response)

-- 客户端生成 foo 的请求数据
local req = client_request("foo", nil, 2)
print("request foo size =", #req)

-- 服务器处理客户端 foo 的请求数据
local type, name, request, response = server:dispatch(req)
assert(type == "REQUEST" and name == "foo" and request == nil)

-- 服务器生成 foo 响应数据
local resp = response { ok = false }
print("response package size =", #resp)

-- 客户端处理 foo 的响应数据
print("client dispatch")
local type, session, response = client:dispatch(resp)
assert(type == "RESPONSE" and session == 2)
print_r(response)

-- 客户端生成 bar 的请求数据
local req = client_request("bar")   -- bar has no response, bar 协议没有响应
print("request bar size =", #req)

-- 服务器处理 bar 的请求数据
local type, name, request, response = server:dispatch(req)
assert(type == "REQUEST" and name == "bar" and request == nil and response == nil)

-- 客户端生成 blackhole 的请求数据
local req = client_request "blackhole"
print("request blackhole size = ", #req)

print("=== test 2")

-- server 对 foobar 协议的 request 进行编码
local v, tag = server_proto:request_encode("foobar", { what = "hello"})
print("tag =", tag)

-- server 对 foobar 的 request 数据进行解码
print_r(server_proto:request_decode("foobar", v))

-- server 对 foobar 协议的 response 进行编码
local v, tag = server_proto:response_encode("foobar", { ok = true })
print("tag =", tag)

-- server 对 foobar 协议的 response 数据进行解码
print_r(server_proto:response_decode("foobar", v))