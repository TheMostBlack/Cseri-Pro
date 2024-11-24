# Cseri Pro

适用于Lua5.x的Lua Table高性能序列化库,基于[Cseri](https://github.com/luyuhuang/cseri),增加了三种可选的压缩算法,用于对序列化数据进行压缩:

[Google Snappy](https://github.com/google/snappy)

[Zlib](https://github.com/madler/zlib)

[Zstd](https://github.com/facebook/zstd)

并支持了对function进行序列化

## Usage

```lua
local cseri = require("cseri")

local data = {
  a = 1,
  b = "2",
  c = {1, 2, 3},
  d = function(a, b) return a+b end
}

local txt = {
  a = 1,
  b = "value"
}

-- Google Snappy压缩,无压缩级别
local bin = cseri.tobin(data, "snappy")
local obj = cseri.frombin(bin, "snappy")
print(obj.d(1, 2)) -- 3

-- Zlib压缩,压缩级别为6,最高为9,最低为1
local bin = cseri.tobin(data, "zlib", 6)
local obj = cseri.frombin(bin, "zlib")

-- Zstd压缩,压缩级别为6,最高为22,最低为-131072
local bin = cseri.tobin(data, "zstd", 6)
local obj = cseri.frombin(bin, "zstd")

-- 不压缩
local bin = cseri.tobin(data, "none")
local obj = cseri.frombin(bin, "none")

-- Table转字符串
print(cseri.totxt(txt, "str")) -- {a=1,b="value"},"str"
```
