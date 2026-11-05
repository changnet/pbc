-- 重新生成 pbc/src/descriptor.pbc.h
--
-- 背景：descriptor.pbc.h 不是 protoc 的 FileDescriptorSet，而是 pbc 自有的
--       `pbc.file` 消息（定义在 src/bootstrap.c 的注释里）序列化后的字节数组。
--       它把 google/protobuf/descriptor.proto 的 FileDescriptorSet 转码成
--       "并行数组" 格式：message_name/message_size/message_field、
--       enum_name/enum_size/enum_string/enum_id，供 _pbcB_init() 用 pattern
--       API 自举出解析 descriptor.proto 所需的 schema。
--
-- 生成链路（与 2011 年 cloudwu 的做法等价，只是把 protoc 换成纯 Lua 编译器）：
--   1. pbc/test/descriptor.proto  --(protoc.lua)-->  FileDescriptorSet 二进制
--   2. pb.decode 成 Lua 表
--   3. 按 pbc.file 的字段定义转码（见 bootstrap.c）
--   4. 输出 C 字节数组头文件
--
-- 用法（在本工程根目录，需要已编译的 bench.exe 提供 pb/protoc 模块）：
--   build/bin/bench.exe tools/gen_descriptor_pbc.lua [descriptor.proto] [输出.h]
-- 不带参数时：读 pbc/test/descriptor.proto，写到 build/descriptor.pbc.h.generated，
--             并与 pbc/src/descriptor.pbc.h 做逐字节比对。

local protoc = require "protoc"
local pb = require "pb"

-- bench.exe 把命令行参数作为 chunk 的 vararg 传入（不设置全局 arg）
local cli = { ... }
local src = cli[1] or "pbc/test/descriptor.proto"
local dst = cli[2] or "build/descriptor.pbc.h.generated"
local ref = "pbc/src/descriptor.pbc.h"

-- ---------------------------------------------------------------------------
-- 1. 编译 descriptor.proto，得到 FileDescriptorSet
-- ---------------------------------------------------------------------------
local f = assert(io.open(src, "rb"))
local proto_text = f:read("a")
f:close()

local compiler = protoc.new()                     -- new() 内部会 pb.load(descriptor_pb)
local fds_bin = compiler:compile(proto_text, "descriptor.proto")

-- 枚举要按数值返回（默认 lua-protobuf 返回枚举名字符串）
pb.option "enum_as_value"
local set = assert(pb.decode(".google.protobuf.FileDescriptorSet", fds_bin))
local fd = assert(set.file[1], "FileDescriptorSet.file is empty")
local pkg = fd.package or ""

-- ---------------------------------------------------------------------------
-- 2. 转码成 pbc.file 的逻辑结构
-- ---------------------------------------------------------------------------
local msgs = {}     -- { name=全限定名, msg=DescriptorProto }，深度优先、嵌套紧跟父
local enums = {}    -- { name=全限定名, enum=EnumDescriptorProto }
local fields = {}   -- 所有字段按 message 顺序摊平

local function walk(m, prefix)
    local full = prefix ~= "" and (prefix .. "." .. m.name) or m.name
    msgs[#msgs + 1] = { name = full, msg = m }
    for _, e in ipairs(m.enum_type or {}) do
        enums[#enums + 1] = { name = full .. "." .. e.name, enum = e }
    end
    for _, n in ipairs(m.nested_type or {}) do
        walk(n, full)
    end
end

for _, m in ipairs(fd.message_type or {}) do walk(m, pkg) end
for _, e in ipairs(fd.enum_type or {}) do
    enums[#enums + 1] = {
        name = pkg ~= "" and (pkg .. "." .. e.name) or e.name,
        enum = e,
    }
end

for _, mi in ipairs(msgs) do
    for _, fld in ipairs(mi.msg.field or {}) do
        fields[#fields + 1] = fld
    end
end

-- 展平枚举值
local values = {}
for _, ei in ipairs(enums) do
    for _, v in ipairs(ei.enum.value or {}) do
        values[#values + 1] = v
    end
end

-- ---------------------------------------------------------------------------
-- 3. pbc.file wire 编码（手写 varint，保持与 2011 年生成器一致的字段顺序）
-- ---------------------------------------------------------------------------
local buf = {}
local function put(s) buf[#buf + 1] = s end

local function varint(n)
    local t = {}
    repeat
        local b = n & 0x7f
        n = n >> 7
        if n ~= 0 then b = b | 0x80 end
        t[#t + 1] = string.char(b)
    until n == 0
    put(table.concat(t))
end

local function tag(fid, wt) varint(fid * 8 + wt) end
local function int(fid, v) tag(fid, 0); varint(v) end
local function str(fid, s)
    s = s .. "\0"                 -- pbc 约定：string 带结尾 NUL
    tag(fid, 2); varint(#s); put(s)
end
local function real(fid, v) tag(fid, 1); put(string.pack("<d", v)) end
local function sub(fid, fn)
    local saved = buf
    buf = {}
    fn()
    local data = table.concat(buf)
    buf = saved
    tag(fid, 2); varint(#data); put(data)
end

local function strip_dot(s)
    if s and s:sub(1, 1) == "." then return s:sub(2) end
    return s
end

local function emit_field(fld)
    local t = tonumber(fld.type) or 0
    local label = (tonumber(fld.label) or 1) - 1   -- protobuf 1/2/3 -> pbc 0/1/2
    local id = tonumber(fld.number) or 0
    local tn = strip_dot(fld.type_name)
    local dv = fld.default_value

    -- 原生成器用 Lua pairs() 遍历 field 表，tag 顺序随「键集合」变化：
    --   带 type_name / default 的记录：3,2,4,[5],1,[6/7/8]
    --   纯标量记录（无额外键）：4,3,1,2
    -- protobuf 允许字段乱序，两者语义相同；这里复刻原顺序以便逐字节比对。
    if tn or dv ~= nil then
        int(3, label)
        int(2, id)
        int(4, t)
        if tn then str(5, tn) end
        str(1, fld.name)
        if dv ~= nil then
            if t == 1 or t == 2 then                   -- double / float
                real(8, tonumber(dv) or 0)
            elseif t == 9 or t == 12 or t == 14 then   -- string / bytes / enum
                str(7, dv)
            else                                       -- 整数 / bool
                local n = tonumber(dv)
                if not n then n = (dv == "true") and 1 or 0 end
                int(6, n)
            end
        end
    else
        int(4, t)
        int(3, label)
        str(1, fld.name)
        int(2, id)
    end
end

-- enum_id(9) 全部展平
for _, v in ipairs(values) do int(9, tonumber(v.number) or 0) end
-- enum_name(6)
for _, ei in ipairs(enums) do str(6, ei.name) end
-- enum_string(8)
for _, v in ipairs(values) do str(8, v.name) end
-- enum_size(7)
for _, ei in ipairs(enums) do int(7, #(ei.enum.value or {})) end
-- name(1)：去掉 .proto 后缀
str(1, (fd.name or "descriptor"):gsub("%.proto$", ""))
-- message_size(4)
for _, mi in ipairs(msgs) do int(4, #(mi.msg.field or {})) end
-- message_field(5)
for _, fld in ipairs(fields) do sub(5, function() emit_field(fld) end) end
-- message_name(3)
for _, mi in ipairs(msgs) do str(3, mi.name) end
-- dependency(2)
for _, d in ipairs(fd.dependency or {}) do str(2, d) end

local data = table.concat(buf)

-- ---------------------------------------------------------------------------
-- 4. 输出 C 头文件（16 个数字一行，末尾无多余换行，与现有 header 一致）
-- ---------------------------------------------------------------------------
local out = { "static unsigned char pbc_descriptor[] = {" }
local line = {}
for i = 1, #data do
    line[#line + 1] = tostring(data:byte(i)) .. ","
    if #line == 16 then
        out[#out + 1] = table.concat(line)
        line = {}
    end
end
if #line > 0 then out[#out + 1] = table.concat(line) end
out[#out + 1] = "};"
local text_out = table.concat(out, "\n")

local o = assert(io.open(dst, "wb"))
o:write(text_out)
o:close()

-- ---------------------------------------------------------------------------
-- 5. 与现有 header 逐字节比对
-- ---------------------------------------------------------------------------
local r = io.open(ref, "rb")
if r then
    local old = r:read("a")
    r:close()
    if old == text_out then
        print(string.format("OK: %s 与现有 header 完全一致（%d 字节 / %d 个 message / %d 个 field / %d 个 enum / %d 个 enum value）",
            dst, #data, #msgs, #fields, #enums, #values))
    else
        local n = math.min(#old, #text_out)
        local at = n + 1
        for i = 1, n do
            if old:byte(i) ~= text_out:byte(i) then at = i; break end
        end
        print(string.format("DIFF: 现有 %d 字节，生成 %d 字节，首个差异在第 %d 字节", #old, #text_out, at))
        print("  现有:", old:sub(math.max(1, at - 40), at + 40):gsub("%c", "."))
        print("  生成:", text_out:sub(math.max(1, at - 40), at + 40):gsub("%c", "."))
    end
else
    print(string.format("生成 %s（%d 字节），未找到 %s 可比", dst, #data, ref))
end
