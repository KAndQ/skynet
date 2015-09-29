-- 主要功能是将 sproto 模式字符串穿转化成二进制数据字符串

--[==[

Wire protocol

Each integer number must be serialized in little-endian format.
每个整型的数字必须是以 little-endian 的方式编码.

The sproto message must be a user defined type struct, and a struct is encoded in three parts. 
The header, the field part, and the data part. The tag and small integer or boolean will be encoded in field part, 
and others are in data part.

sproto 消息数据必须是 1 个用户定义的结构类型, 并且 1 个结构被 3 个部分编码.
分别是"头"部分, "字段"部份, "数据"部分. 标记(tag)和小的整型或者布尔类型将被编码在"字段"部分, 
但是其他的将被编码在"数据"部分.

All the fields must be encoded in ascending order (by tag). The tags of fields can be discontinuous, 
if a field is nil. (default value in lua), don't encode it in message.
所有的"字段"根据标记以升序编码. "字段"的标记可以是不连续的, 如果 1 个"字段"的值是 nil(lua 中的默认值), 那么不会把该"字段"编码进消息数据中.

The header is a 16bit integer. It is the number of fields.
"头"部分是 16 位的整型. 表示的是字段的数量.

Each field in field part is a 16bit integer (n). If n is zero, that means the field data is encoded in data part ;
每个字段在"字段"部分是 16 位的整型(n). 如果 n 是 0, 意味着字段的数据被编码在"数据"部分;

If n is even (and not zero), the value of this field is n/2-1 ;
如果 n 是偶数(并且非 0), 这个"字段"的值是 n/2-1;

If n is odd, that means the tags is not continuous, and we should add current tag by (n+1)/2 .
如果 n 是基数, 意味着标记是不连续的, 但是我们仍应该使用 (n+1)/2 的值作为当前的标记.

Notice: If the tag is not declared in schema, the decoder will simply ignore the field for protocol version compatibility.
注意: 如果标记没有在模式中声明, 那么解码时将直接忽略掉这个字段, 这样做为的协议版本的兼容性.

--]==]

local lpeg = require "lpeg"
local table = require "table"

-- 打包字符串
local packbytes

-- 打包值
local packvalue

if _VERSION == "Lua 5.3" then
	function packbytes(str)
		-- <: 设为小端编码
		return string.pack("<s4",str)	-- s[n]: 长度加内容的字符串，其长度编码为一个 n 字节（默认是个 size_t） 长的无符号整数。
	end

	function packvalue(id)
		id = (id + 1) * 2

		-- <: 设为小端编码
		return string.pack("<I2",id)	-- I[n]: 一个 n 字节长（默认为本地大小）的无符号 int
	end
else
	function packbytes(str)
		local size = #str
		local a = size % 256
		size = math.floor(size / 256)
		local b = size % 256
		size = math.floor(size / 256)
		local c = size % 256
		size = math.floor(size / 256)
		local d = size
		return string.char(a) .. string.char(b) .. string.char(c) .. string.char(d) .. str
	end

	function packvalue(id)
		id = (id + 1) * 2
		assert(id >=0 and id < 65536)
		local a = id % 256
		local b = math.floor(id / 256)
		return string.char(a) .. string.char(b)
	end
end

-- Converts the given value into a proper pattern, according to the following rules:
-- 
local P = lpeg.P
local S = lpeg.S
local R = lpeg.R
local C = lpeg.C
local Ct = lpeg.Ct
local Cg = lpeg.Cg
local Cc = lpeg.Cc
local V = lpeg.V

local function count_lines(_, pos, parser_state)
	if parser_state.pos < pos then
		parser_state.line = parser_state.line + 1
		parser_state.pos = pos
	end
	return pos
end

local exception = lpeg.Cmt( lpeg.Carg(1) , function ( _ , pos, parser_state)
	error(string.format("syntax error at [%s] line (%d)", parser_state.file or "", parser_state.line))
	return pos
end)

local eof = P(-1)
local newline = lpeg.Cmt((P"\n" + "\r\n") * lpeg.Carg(1), count_lines)
local line_comment = "#" * (1 - newline) ^0 * (newline + eof)
local blank = S" \t" + newline + line_comment
local blank0 = blank ^ 0
local blanks = blank ^ 1
local alpha = R"az" + R"AZ" + "_"
local alnum = alpha + R"09"
local word = alpha * alnum ^ 0
local name = C(word)
local typename = C(word * ("." * word) ^ 0)
local tag = R"09" ^ 1 / tonumber
local mainkey = "(" * blank0 * name * blank0 * ")"

local function multipat(pat)
	return Ct(blank0 * (pat * blanks) ^ 0 * pat^0 * blank0)
end

local function namedpat(name, pat)
	return Ct(Cg(Cc(name), "type") * Cg(pat))
end

local typedef = P {
	"ALL",
	FIELD = namedpat("field", (name * blanks * tag * blank0 * ":" * blank0 * (C"*")^-1 * typename * mainkey^0)),
	STRUCT = P"{" * multipat(V"FIELD" + V"TYPE") * P"}",
	TYPE = namedpat("type", P"." * name * blank0 * V"STRUCT" ),
	SUBPROTO = Ct((C"request" + C"response") * blanks * (typename + V"STRUCT")),
	PROTOCOL = namedpat("protocol", name * blanks * tag * blank0 * P"{" * multipat(V"SUBPROTO") * P"}"),
	ALL = multipat(V"TYPE" + V"PROTOCOL"),
}

local proto = blank0 * typedef * blank0

local convert = {}

function convert.protocol(all, obj)
	local result = { tag = obj[2] }
	for _, p in ipairs(obj[3]) do
		assert(result[p[1]] == nil)
		local typename = p[2]
		if type(typename) == "table" then
			local struct = typename
			typename = obj[1] .. "." .. p[1]
			all.type[typename] = convert.type(all, { typename, struct })
		end
		result[p[1]] = typename
	end
	return result
end

function convert.type(all, obj)
	local result = {}
	local typename = obj[1]
	local tags = {}
	local names = {}
	for _, f in ipairs(obj[2]) do
		if f.type == "field" then
			local name = f[1]
			if names[name] then
				error(string.format("redefine %s in type %s", name, typename))
			end
			names[name] = true
			local tag = f[2]
			if tags[tag] then
				error(string.format("redefine tag %d in type %s", tag, typename))
			end
			tags[tag] = true
			local field = { name = name, tag = tag }
			table.insert(result, field)
			local fieldtype = f[3]
			if fieldtype == "*" then
				field.array = true
				fieldtype = f[4]
			end
			local mainkey = f[5]
			if mainkey then
				assert(field.array)
				field.key = mainkey
			end
			field.typename = fieldtype
		else
			assert(f.type == "type")	-- nest type
			local nesttypename = typename .. "." .. f[1]
			f[1] = nesttypename
			assert(all.type[nesttypename] == nil, "redefined " .. nesttypename)
			all.type[nesttypename] = convert.type(all, f)
		end
	end
	table.sort(result, function(a,b) return a.tag < b.tag end)
	return result
end

local function adjust(r)
	local result = { type = {} , protocol = {} }

	for _, obj in ipairs(r) do
		local set = result[obj.type]
		local name = obj[1]
		assert(set[name] == nil , "redefined " .. name)
		set[name] = convert[obj.type](result,obj)
	end

	return result
end

-- 内置类型定义
local buildin_types = {
	integer = 0,	-- 整型
	boolean = 1,	-- 布尔
	string = 2,		-- 字符串类型
}

local function checktype(types, ptype, t)
	if buildin_types[t] then
		return t
	end
	local fullname = ptype .. "." .. t
	if types[fullname] then
		return fullname
	else
		ptype = ptype:match "(.+)%..+$"
		if ptype then
			return checktype(types, ptype, t)
		elseif types[t] then
			return t
		end
	end
end

local function check_protocol(r)
	local map = {}
	local type = r.type
	for name, v in pairs(r.protocol) do
		local tag = v.tag
		local request = v.request
		local response = v.response
		local p = map[tag]

		if p then
			error(string.format("redefined protocol tag %d at %s", tag, name))
		end

		if request and not type[request] then
			error(string.format("Undefined request type %s in protocol %s", request, name))
		end

		if response and not type[response] then
			error(string.format("Undefined response type %s in protocol %s", response, name))
		end

		map[tag] = v
	end
	return r
end

local function flattypename(r)
	for typename, t in pairs(r.type) do
		for _, f in pairs(t) do
			local ftype = f.typename
			local fullname = checktype(r.type, typename, ftype)
			if fullname == nil then
				error(string.format("Undefined type %s in type %s", ftype, typename))
			end
			f.typename = fullname
		end
	end

	return r
end

local function parser(text, filename)
	local state = { file = filename, pos = 0, line = 1 }

	-- The matching function. It attempts to match the given pattern against the subject string. 
	-- If the match succeeds, returns the index in the subject of the first character after the match, 
	-- or the captured values (if the pattern captured any value).
	-- 匹配函数. 它试图找到匹配的字符串传. 如果匹配成功, 返回产生匹配的第一个字符串的位置, 或者捕获的值(如果模式捕获若干个值).
	local r = lpeg.match(proto * -1 + exception, text, 1, state)
	return flattypename(check_protocol(adjust(r)))
end

--[[
-- The protocol of sproto
.type {
	.field {
		name 0 : string
		buildin	1 :	integer
		type 2 : integer
		tag	3 :	integer
		array 4	: boolean
		key 5 : integer # If key exists, array must be true, and it's a map.
	}
	name 0 : string
	fields 1 : *field
}

.protocol {
	name 0 : string
	tag	1 :	integer
	request	2 :	integer	# index
	response 3 : integer # index
}

.group {
	type 0 : *type
	protocol 1 : *protocol
}
]]

local function packfield(f)
	local strtbl = {}
	if f.array then
		if f.key then
			table.insert(strtbl, "\6\0")  -- 6 fields
		else
			table.insert(strtbl, "\5\0")  -- 5 fields
		end
	else
		table.insert(strtbl, "\4\0")	-- 4 fields
	end
	table.insert(strtbl, "\0\0")	-- name	(tag = 0, ref an object)
	if f.buildin then
		table.insert(strtbl, packvalue(f.buildin))	-- buildin (tag = 1)
		table.insert(strtbl, "\1\0")	-- skip (tag = 2)
		table.insert(strtbl, packvalue(f.tag))		-- tag (tag = 3)
	else
		table.insert(strtbl, "\1\0")	-- skip (tag = 1)
		table.insert(strtbl, packvalue(f.type))		-- type (tag = 2)
		table.insert(strtbl, packvalue(f.tag))		-- tag (tag = 3)
	end
	if f.array then
		table.insert(strtbl, packvalue(1))	-- array = true (tag = 4)
	end
	if f.key then
		table.insert(strtbl, packvalue(f.key)) -- key tag (tag = 5)
	end
	table.insert(strtbl, packbytes(f.name)) -- external object (name)
	return packbytes(table.concat(strtbl))
end

local function packtype(name, t, alltypes)
	local fields = {}
	local tmp = {}
	for _, f in ipairs(t) do
		tmp.array = f.array
		tmp.name = f.name
		tmp.tag = f.tag

		tmp.buildin = buildin_types[f.typename]
		local subtype
		if not tmp.buildin then
			subtype = assert(alltypes[f.typename])
			tmp.type = subtype.id
		else
			tmp.type = nil
		end
		if f.key then
			tmp.key = subtype.fields[f.key]
			if not tmp.key then
				error("Invalid map index :" .. f.key)
			end
		else
			tmp.key = nil
		end

		table.insert(fields, packfield(tmp))
	end
	local data
	if #fields == 0 then
		data = {
			"\1\0",	-- 1 fields
			"\0\0",	-- name	(id = 0, ref = 0)
			packbytes(name),
		}
	else
		data = {
			"\2\0",	-- 2 fields
			"\0\0",	-- name	(tag = 0, ref = 0)
			"\0\0", -- field[]	(tag = 1, ref = 1)
			packbytes(name),
			packbytes(table.concat(fields)),
		}
	end

	return packbytes(table.concat(data))
end

local function packproto(name, p, alltypes)
	if p.request then
		local request = alltypes[p.request]
		if request == nil then
			error(string.format("Protocol %s request type %s not found", name, p.request))
		end
		request = request.id
	end
	local tmp = {
		"\4\0",	-- 4 fields
		"\0\0",	-- name (id=0, ref=0)
		packvalue(p.tag),	-- tag (tag=1)
	}
	if p.request == nil and p.response == nil then
		tmp[1] = "\2\0"
	else
		if p.request then
			table.insert(tmp, packvalue(alltypes[p.request].id)) -- request typename (tag=2)
		else
			table.insert(tmp, "\1\0")
		end
		if p.response then
			table.insert(tmp, packvalue(alltypes[p.response].id)) -- request typename (tag=3)
		else
			tmp[1] = "\3\0"
		end
	end

	table.insert(tmp, packbytes(name))

	return packbytes(table.concat(tmp))
end

local function packgroup(t,p)
	if next(t) == nil then
		assert(next(p) == nil)
		return "\0\0"
	end
	local tt, tp
	local alltypes = {}
	for name in pairs(t) do
		table.insert(alltypes, name)
	end
	table.sort(alltypes)	-- make result stable
	for idx, name in ipairs(alltypes) do
		local fields = {}
		for _, type_fields in ipairs(t[name]) do
			if buildin_types[type_fields.typename] then
				fields[type_fields.name] = type_fields.tag
			end
		end
		alltypes[name] = { id = idx - 1, fields = fields }
	end
	tt = {}
	for _,name in ipairs(alltypes) do
		table.insert(tt, packtype(name, t[name], alltypes))
	end
	tt = packbytes(table.concat(tt))
	if next(p) then
		local tmp = {}
		for name, tbl in pairs(p) do
			table.insert(tmp, tbl)
			tbl.name = name
		end
		table.sort(tmp, function(a,b) return a.tag < b.tag end)

		tp = {}
		for _, tbl in ipairs(tmp) do
			table.insert(tp, packproto(tbl.name, tbl, alltypes))
		end
		tp = packbytes(table.concat(tp))
	end
	local result
	if tp == nil then
		result = {
			"\1\0",	-- 1 field
			"\0\0",	-- type[] (id = 0, ref = 0)
			tt,
		}
	else
		result = {
			"\2\0",	-- 2fields
			"\0\0",	-- type array	(id = 0, ref = 0)
			"\0\0",	-- protocol array	(id = 1, ref =1)

			tt,
			tp,
		}
	end

	return table.concat(result)
end

local function encodeall(r)
	return packgroup(r.type, r.protocol)
end

local sparser = {}

-- 将 str 字符串的每个字符以 16 进制的格式输出
-- @param str 字符串对象
function sparser.dump(str)
	local tmp = ""
	for i = 1, #str do
		tmp = tmp .. string.format("%02X ", string.byte(str,i))
		if i % 8 == 0 then
			if i % 16 == 0 then		-- 每 16 个字节打印输出一次
				print(tmp)
				tmp = ""
			else
				tmp = tmp .. "- "	-- 每 8 个字节添加一个 "-" 分割符号
			end
		end
	end
	print(tmp)
end

-- 将 sproto 模式的字符串转化为二进制字符串.
-- @param text sproto 模式字符串
-- @param name 用于 lpeg.match 的参数, 一般不传
-- @return 转化后的二进制字符串
function sparser.parse(text, name)
	local r = parser(text, name or "=text")
	local data = encodeall(r)
	return data
end

return sparser
