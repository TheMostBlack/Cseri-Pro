#include <lauxlib.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h> // Zlib
#include <snappy-c.h> // Google Snappy
#include <zstd.h> // Zstd
#include "common.h"
#include "buffer.h"

#define TYPE_NIL 0
#define TYPE_BOOLEAN 1
// hibits 0 false 1 true

#define TYPE_NUMBER 2
// hibits 0 : 0 , 1: byte, 2:word, 4: dword, 6: qword, 8 : double
#define TYPE_NUMBER_ZERO 0
#define TYPE_NUMBER_BYTE 1
#define TYPE_NUMBER_WORD 2
#define TYPE_NUMBER_DWORD 4
#define TYPE_NUMBER_QWORD 6
#define TYPE_NUMBER_REAL 8

#define TYPE_USERDATA 3
#define TYPE_SHORT_STRING 4
// hibits 0~31 : len
#define TYPE_LONG_STRING 5
#define TYPE_TABLE 6
#define TYPE_FUNCTION 7

#define MAX_COOKIE 32
#define COMBINE_TYPE(t,v) ((t) | (v) << 3)

#define buffer_append(bf, data, len) buffer_append(bf, (char*)data, len)

#define MAX_LUA_INTEGER  (1ULL << (sizeof(lua_Integer) * 8 - 1)) - 1

/* dummy union to get native endianness */
static const union {
  int dummy;
  char little;  /* true iff machine is little endian */
} nativeendian = {1};

inline static void
_convert(char *p, size_t size) {
    if (!nativeendian.little) return;
    for (size_t i = 0; i < size / 2; ++i) {
        char t = p[i];
        p[i] = p[size - i - 1];
        p[size - i - 1] = t;
    }
}

#define CONVERT(n) _convert((char*)&(n), sizeof(n))

static inline void append_nil(struct buffer *bf) {
    uint8_t n = TYPE_NIL;
    buffer_append(bf, (char*)&n, 1);
}

static inline void append_boolean(struct buffer *bf, int boolean) {
    uint8_t n = COMBINE_TYPE(TYPE_BOOLEAN , boolean ? 1 : 0);
    buffer_append(bf, (char*)&n, 1);
}

static inline void append_integer(struct buffer *bf, int64_t v) {
    uint8_t n = COMBINE_TYPE(TYPE_NUMBER, 0);
    if (v == 0) {
        n = COMBINE_TYPE(TYPE_NUMBER, TYPE_NUMBER_ZERO);
        buffer_append(bf, (char*)&n, 1);
    } else if (v != (int32_t)v) {
        n = COMBINE_TYPE(TYPE_NUMBER, TYPE_NUMBER_QWORD);
        int64_t v64 = v;
        CONVERT(v64);
        buffer_append(bf, (char*)&n, 1);
        buffer_append(bf, (char*)&v64, sizeof(v64));
    } else if (v < 0) {
        int32_t v32 = (int32_t)v;
        CONVERT(v32);
        n = COMBINE_TYPE(TYPE_NUMBER, TYPE_NUMBER_DWORD);
        buffer_append(bf, (char*)&n, 1);
        buffer_append(bf, (char*)&v32, sizeof(v32));
    } else if (v < 0x100) {
        uint8_t byte = (uint8_t)v;
        n = COMBINE_TYPE(TYPE_NUMBER, TYPE_NUMBER_BYTE);
        buffer_append(bf, (char*)&n, 1);
        buffer_append(bf, (char*)&byte, sizeof(byte));
    } else if (v < 0x10000) {
        uint16_t word = (uint16_t)v;
        CONVERT(word);
        n = COMBINE_TYPE(TYPE_NUMBER, TYPE_NUMBER_WORD);
        buffer_append(bf, (char*)&n, 1);
        buffer_append(bf, (char*)&word, sizeof(word));
    } else {
        uint32_t v32 = (uint32_t)v;
        CONVERT(v32);
        n = COMBINE_TYPE(TYPE_NUMBER, TYPE_NUMBER_DWORD);
        buffer_append(bf, (char*)&n, 1);
        buffer_append(bf, (char*)&v32, sizeof(v32));
    }
}

static inline void append_real(struct buffer *bf, double v) {
    uint8_t n = COMBINE_TYPE(TYPE_NUMBER , TYPE_NUMBER_REAL);
    buffer_append(bf, (char*)&n, 1);
    buffer_append(bf, (char*)&v, sizeof(v));
}

static inline void append_string(struct buffer *bf, const char *str, int len) {
    if (len < MAX_COOKIE) {
        uint8_t n = COMBINE_TYPE(TYPE_SHORT_STRING, len);
        buffer_append(bf, (char*)&n, 1);
        if (len > 0) {
            buffer_append(bf, str, len);
        }
    } else {
        uint8_t n;
        if (len < 0x10000) {
            n = COMBINE_TYPE(TYPE_LONG_STRING, 2);
            buffer_append(bf, (char*)&n, 1);
            uint16_t x = (uint16_t)len;
            CONVERT(x);
            buffer_append(bf, (char*)&x, 2);
        } else {
            n = COMBINE_TYPE(TYPE_LONG_STRING, 4);
            buffer_append(bf, (char*)&n, 1);
            uint32_t x = (uint32_t) len;
            CONVERT(x);
            buffer_append(bf, (char*)&x, 4);
        }
        buffer_append(bf, str, len);
    }
}

static inline void append_function(struct buffer *bf, const char *bytecode, int len) {
    if (len < MAX_COOKIE) {
        uint8_t n = COMBINE_TYPE(TYPE_FUNCTION, len);
        buffer_append(bf, (char*)&n, 1);
        if (len > 0) {
            buffer_append(bf, bytecode, len);
        }
    } else {
        uint8_t n = COMBINE_TYPE(TYPE_FUNCTION, 0);
        buffer_append(bf, (char*)&n, 1);
        append_integer(bf, len);
        buffer_append(bf, bytecode, len);
    }
}

static void pack_one(lua_State *L, struct buffer *b, int index, int depth);

static int
append_table_array(lua_State *L, struct buffer *bf, int index, int depth) {
    int array_size = lua_rawlen(L,index);
    if (array_size >= MAX_COOKIE-1) {
        uint8_t n = COMBINE_TYPE(TYPE_TABLE, MAX_COOKIE-1);
        buffer_append(bf, (char*)&n, 1);
        append_integer(bf, array_size);
    } else {
        uint8_t n = COMBINE_TYPE(TYPE_TABLE, array_size);
        buffer_append(bf, (char*)&n, 1);
    }

    int i;
    for (i=1;i<=array_size;i++) {
        lua_rawgeti(L,index,i);
        pack_one(L, bf, -1, depth + 1);
        lua_pop(L, 1);
    }

    return array_size;
}

static void
append_table_hash(lua_State *L, struct buffer *bf, int index, int depth, int array_size) {
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L,-2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
            lua_Integer i = lua_tointeger(L, -2);
            if (i > 0 && i <= array_size) {
                lua_pop(L,1);
                continue;
            }
        }
        pack_one(L,bf,-2, depth +1);
        pack_one(L,bf,-1, depth +1);
        lua_pop(L, 1);
    }
    append_nil(bf);
}

static void
pack_table(lua_State *L, struct buffer *bf, int index, int depth) {
    luaL_checkstack(L, LUA_MINSTACK, NULL);
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    int array_size = append_table_array(L, bf, index, depth);
    append_table_hash(L, bf, index, depth, array_size);
}

static int writer_lua_dump(lua_State *L, const void* p, size_t sz, void* ud) {
    struct buffer *bf = (struct buffer *)ud;
    buffer_append(bf, (const char *)p, sz);
    return 0;
}

static void
pack_function(lua_State *L, struct buffer *bf, int index, int depth) {
    struct buffer func_bf;
    buffer_initialize(&func_bf, L);

    lua_pushvalue(L, index);

    if (lua_dump(L, writer_lua_dump, &func_bf, 0) != 0) {
        buffer_free(&func_bf);
        buffer_free(bf);
        luaL_error(L, "函数编译失败");
    }

    size_t sz = buffer_size(&func_bf);
    if (sz == 0) {
        buffer_free(&func_bf);
        lua_pop(L, 1);
        luaL_error(L, "函数字节码为空");
    }

    char *bytecode = (char*)malloc(sz);
    if (!bytecode) {
        buffer_free(&func_bf);
        buffer_free(bf);
        lua_pop(L, 1);
        luaL_error(L, "内存分配失败");
    }

    struct block *p = func_bf.head;
    size_t pos = 0;
    while (p) {
        memcpy(bytecode + pos, p->data, p->p);
        pos += p->p;
        p = p->next;
    }

    append_function(bf, bytecode, sz);

    buffer_free(&func_bf);
    free(bytecode);
    lua_pop(L, 1);
}

static void
pack_one(lua_State *L, struct buffer *b, int index, int depth) {
    if (depth > MAX_DEPTH) {
        buffer_free(b);
        luaL_error(L, "serialize can't pack too depth table");
    }
    int type = lua_type(L,index);
    switch(type) {
    case LUA_TNIL:
        append_nil(b);
        break;
    case LUA_TNUMBER: {
        if (lua_isinteger(L, index)) {
            lua_Integer x = lua_tointeger(L,index);
            append_integer(b, x);
        } else {
            lua_Number n = lua_tonumber(L,index);
            append_real(b,n);
        }
        break;
    }
    case LUA_TBOOLEAN:
        append_boolean(b, lua_toboolean(L,index));
        break;
    case LUA_TSTRING: {
        size_t sz = 0;
        const char *str = lua_tolstring(L,index,&sz);
        append_string(b, str, (int)sz);
        break;
    }
    case LUA_TTABLE: {
        if (index < 0) {
            index = lua_gettop(L) + index + 1;
        }
        pack_table(L, b, index, depth+1);
        break;
    }
    case LUA_TFUNCTION: {
        if (index < 0) {
            index = lua_gettop(L) + index + 1;
        }
        pack_function(L, b, index, depth+1);
        break;
    }
    default:
        buffer_free(b);
        luaL_error(L, "Unsupport type %s to serialize", lua_typename(L, type));
    }
}

static char *buffer_to_string(struct buffer *b, size_t *size) {
    *size = buffer_size(b);
    void *ud;
    lua_Alloc alloc = lua_getallocf(b->L, &ud);
    char *str = (char *)alloc(ud, NULL, 0, *size);
    if (!str) return NULL;
    char *s = str;
    struct block *p = b->head;
    while (p) {
        memcpy(s, p->data, p->p);
        s += p->p;
        p = p->next;
    }
    return str;
}

int to_bin(lua_State *L) {
    int arg_top = lua_gettop(L);
    int compression_level = 1; // 默认压缩级别为1
    const char *compression_type = "snappy"; // 默认使用Snappy压缩

    // 判断是否传入了压缩级别和压缩方式
    if (lua_type(L, arg_top) == LUA_TNUMBER) {
        compression_level = lua_tointeger(L, arg_top);
        --arg_top;
    }
    if (lua_type(L, arg_top) == LUA_TSTRING) {
        compression_type = lua_tostring(L, arg_top);
        --arg_top;
    } else if (lua_type(L, arg_top) == LUA_TBOOLEAN) {
        // 如果传入了false,则不压缩
        if (!lua_toboolean(L, arg_top)) {
            compression_type = "none";
        }
        --arg_top;
    }

    struct buffer bf;
    buffer_initialize(&bf, L);

    for (int i = 1; i <= arg_top; ++i) {
        pack_one(L, &bf, i, 0);
    }

    size_t uncompressed_size;
    char *uncompressed_data = buffer_to_string(&bf, &uncompressed_size);
    if (!uncompressed_data) {
        buffer_free(&bf);
        return luaL_error(L, "内存分配失败");
    }

    if (strcasecmp(compression_type, "snappy") == 0) {
        // Google Snappy
        size_t max_compressed_size = snappy_max_compressed_length(uncompressed_size);
        char *compressed_data = (char *)malloc(max_compressed_size);
        if (compressed_data == NULL) {
            buffer_free(&bf);
            free(uncompressed_data);
            return luaL_error(L, "内存分配失败");
        }
        size_t compressed_size = max_compressed_size;
        snappy_status res = snappy_compress(uncompressed_data, uncompressed_size, compressed_data, &compressed_size);
        if (res != SNAPPY_OK) {
            buffer_free(&bf);
            free(uncompressed_data);
            free(compressed_data);
            return luaL_error(L, "Snappy压缩失败");
        }

        lua_pushlstring(L, compressed_data, compressed_size);
        free(compressed_data);
    } else if (strcasecmp(compression_type, "zlib") == 0) {
        // Zlib
        if (compression_level < Z_BEST_SPEED || compression_level > Z_BEST_COMPRESSION) {
            buffer_free(&bf);
            free(uncompressed_data);
            return luaL_error(L, "Zlib压缩级别最低为%d, 最高为%d", Z_BEST_SPEED, Z_BEST_COMPRESSION);
        }
        uLongf compressed_size = compressBound(uncompressed_size);
        char *compressed_data = (char *)malloc(compressed_size);
        if (compressed_data == NULL) {
            buffer_free(&bf);
            free(uncompressed_data);
            return luaL_error(L, "内存分配失败");
        }
        int res = compress2((Bytef *)compressed_data, &compressed_size, (Bytef *)uncompressed_data, uncompressed_size, compression_level);
        if (res != Z_OK) {
            buffer_free(&bf);
            free(uncompressed_data);
            free(compressed_data);
            return luaL_error(L, "Zlib压缩失败");
        }

        lua_pushlstring(L, compressed_data, compressed_size);
        free(compressed_data);
    } else if (strcasecmp(compression_type, "zstd") == 0) {
        // Zstd
        int zstd_min_level = ZSTD_minCLevel();
        int zstd_max_level = ZSTD_maxCLevel();
        if (compression_level < zstd_min_level || compression_level > zstd_max_level) {
            buffer_free(&bf);
            free(uncompressed_data);
            return luaL_error(L, "Zstd压缩级别最低为%d, 最高为%d", zstd_min_level, zstd_max_level);
        }
        size_t compressed_size = ZSTD_compressBound(uncompressed_size);
        char *compressed_data = (char *)malloc(compressed_size);
        if (compressed_data == NULL) {
            buffer_free(&bf);
            free(uncompressed_data);
            return luaL_error(L, "内存分配失败");
        }
        size_t res = ZSTD_compress(compressed_data, compressed_size, uncompressed_data, uncompressed_size, compression_level);
        if (ZSTD_isError(res)) {
            buffer_free(&bf);
            free(uncompressed_data);
            free(compressed_data);
            return luaL_error(L, "Zstd压缩失败: %s", ZSTD_getErrorName(res));
        }
        compressed_size = res;
        lua_pushlstring(L, compressed_data, compressed_size);
        free(compressed_data);
    } else if (strcasecmp(compression_type, "none") == 0 || strcasecmp(compression_type, "no") == 0) {
        // 不压缩
        lua_pushlstring(L, uncompressed_data, uncompressed_size);
    } else {
        buffer_free(&bf);
        free(uncompressed_data);
        return luaL_error(L, "未知的压缩类型: %s", compression_type);
    }

    buffer_free(&bf);
    free(uncompressed_data);

    return 1;
}

struct reader {
    const char *buffer;
    int len;
    int ptr;
};

static void reader_init(struct reader *rd, const char *buffer, int size) {
    rd->buffer = buffer;
    rd->len = size;
    rd->ptr = 0;
}

static const void *reader_read(struct reader *rd, int size) {
    if (rd->len < size)
        return NULL;

    int ptr = rd->ptr;
    rd->ptr += size;
    rd->len -= size;
    return rd->buffer + ptr;
}

static inline void
invalid_stream_line(lua_State *L, struct reader *rd, int line) {
    luaL_error(L, "Invalid serialize stream %d (line:%d)", rd->ptr, line);
}

#define invalid_stream(L,rd) invalid_stream_line(L,rd,__LINE__)

static int64_t
get_integer(lua_State *L, struct reader *rd, int cookie) {
    switch (cookie) {
    case TYPE_NUMBER_ZERO:
        return 0;
    case TYPE_NUMBER_BYTE: {
        uint8_t n = 0;
        const uint8_t *pn = reader_read(rd, sizeof(n));
        if (pn == NULL)
            invalid_stream(L,rd);
        n = *pn;
        return n;
    }
    case TYPE_NUMBER_WORD: {
        uint16_t n = 0;
        const uint16_t *pn = reader_read(rd, sizeof(n));
        if (pn == NULL)
            invalid_stream(L,rd);
        memcpy(&n, pn, sizeof(n));
        CONVERT(n);
        return n;
    }
    case TYPE_NUMBER_DWORD: {
        int32_t n = 0;
        const int32_t *pn = reader_read(rd, sizeof(n));
        if (pn == NULL)
            invalid_stream(L,rd);
        memcpy(&n, pn, sizeof(n));
        CONVERT(n);
        return n;
    }
    case TYPE_NUMBER_QWORD: {
        int64_t n=0;
        const int64_t *pn = reader_read(rd, sizeof(n));
        if (pn == NULL)
            invalid_stream(L,rd);
        memcpy(&n, pn, sizeof(n));
        CONVERT(n);
        return n;
    }
    default:
        invalid_stream(L,rd);
        return 0;
    }
}

static double
get_real(lua_State *L, struct reader *rd) {
    double n = 0;
    const double *pn = reader_read(rd, sizeof(n));
    if (pn == NULL)
        invalid_stream(L,rd);
    memcpy(&n, pn, sizeof(n));
    return n;
}

static void
get_buffer(lua_State *L, struct reader *rd, int len) {
    const char *p = reader_read(rd, len);
    if (p == NULL) {
        invalid_stream(L, rd);
    }
    lua_pushlstring(L, p, len);
}

static void unpack_one(lua_State *L, struct reader *rd);

static void
unpack_table(lua_State *L, struct reader *rd, int array_size) {
    if (array_size == MAX_COOKIE-1) {
        uint8_t type = 0;
        const uint8_t *t = reader_read(rd, sizeof(type));
        if (t == NULL) {
            invalid_stream(L,rd);
        }
        type = *t;
        int cookie = type >> 3;
        if ((type & 7) != TYPE_NUMBER || cookie == TYPE_NUMBER_REAL) {
            invalid_stream(L,rd);
        }
        array_size = get_integer(L, rd, cookie);
    }
    luaL_checkstack(L,LUA_MINSTACK,NULL);
    lua_createtable(L,array_size,0);
    int i;
    for (i=1;i<=array_size;i++) {
        unpack_one(L,rd);
        lua_rawseti(L,-2,i);
    }
    for (;;) {
        unpack_one(L,rd);
        if (lua_isnil(L,-1)) {
            lua_pop(L,1);
            return;
        }
        unpack_one(L,rd);
        lua_rawset(L,-3);
    }
}

static void
push_function(lua_State *L, struct reader *rd, int len) {
    get_buffer(L, rd, len);
    size_t sz;
    const char *bytecode = lua_tolstring(L, -1, &sz);
    if (luaL_loadbuffer(L, bytecode, sz, "function") != 0) {
        luaL_error(L, "加载函数失败: %s", lua_tostring(L, -1));
    }
    lua_remove(L, -2);
}

static void
push_value(lua_State *L, struct reader *rd, int type, int cookie) {
    switch(type) {
    case TYPE_NIL:
        lua_pushnil(L);
        break;
    case TYPE_BOOLEAN:
        lua_pushboolean(L,cookie);
        break;
    case TYPE_NUMBER:
        if (cookie == TYPE_NUMBER_REAL) {
            lua_pushnumber(L,get_real(L,rd));
        } else {
            int64_t n = get_integer(L, rd, cookie);
            if (llabs(n) > MAX_LUA_INTEGER) {
                lua_pushnumber(L, (lua_Number)n);
            } else {
                lua_pushinteger(L, (lua_Integer)n);
            }
        }
        break;
    case TYPE_SHORT_STRING:
        get_buffer(L,rd,cookie);
        break;
    case TYPE_LONG_STRING: {
        if (cookie == 2) {
            const uint16_t *plen = reader_read(rd, 2);
            if (plen == NULL) {
                invalid_stream(L,rd);
            }
            uint16_t n;
            memcpy(&n, plen, sizeof(n));
            CONVERT(n);
            get_buffer(L,rd,n);
        } else {
            if (cookie != 4) {
                invalid_stream(L,rd);
            }
            const uint32_t *plen = reader_read(rd, 4);
            if (plen == NULL) {
                invalid_stream(L,rd);
            }
            uint32_t n;
            memcpy(&n, plen, sizeof(n));
            CONVERT(n);
            get_buffer(L,rd,n);
        }
        break;
    }
    case TYPE_TABLE: {
        unpack_table(L,rd,cookie);
        break;
    }
    case TYPE_FUNCTION: {
        int len = cookie;
        if (cookie == 0) {
            uint8_t type = 0;
            const uint8_t *t = reader_read(rd, sizeof(type));
            if (t == NULL) {
                invalid_stream(L, rd);
            }
            type = *t;
            int cookie_inner = type >> 3;
            if ((type & 7) != TYPE_NUMBER || cookie_inner == TYPE_NUMBER_REAL) {
                invalid_stream(L, rd);
            }
            len = get_integer(L, rd, cookie_inner);
        }
        push_function(L, rd, len);
        break;
    }
    default: {
        invalid_stream(L, rd);
        break;
    }
    }
}

static void
unpack_one(lua_State *L, struct reader *rd) {
    const uint8_t *t = reader_read(rd, sizeof(uint8_t));
    if (t==NULL) {
        invalid_stream(L, rd);
    }
    push_value(L, rd, *t & 0x7, *t >> 3);
}

int from_bin(lua_State *L) {
    size_t len;
    const char *compressed_data = luaL_checklstring(L, 1, &len);
    const char *compression_type = "snappy"; // 默认使用Snappy解压

    // 判断是否传入了压缩方式参数
    if (lua_gettop(L) >= 2) {
        if (lua_type(L, 2) == LUA_TSTRING) {
            compression_type = lua_tostring(L, 2);
        } else if (lua_type(L, 2) == LUA_TBOOLEAN) {
            // 如果传入了false,则不解压
            if (!lua_toboolean(L, 2)) {
                compression_type = "none";
            }
        }
    }

    char *decompressed_data = NULL;
    size_t decompressed_size = 0;

    if (strcasecmp(compression_type, "snappy") == 0) {
        // Google Snappy
        snappy_status res = snappy_uncompressed_length(compressed_data, len, &decompressed_size);
        if (res != SNAPPY_OK) {
            return luaL_error(L, "无法获取Snappy解压后的长度");
        }

        decompressed_data = (char *)malloc(decompressed_size);
        if (decompressed_data == NULL) {
            return luaL_error(L, "内存分配失败");
        }

        res = snappy_uncompress(compressed_data, len, decompressed_data, &decompressed_size);
        if (res != SNAPPY_OK) {
            free(decompressed_data);
            return luaL_error(L, "Snappy解压失败");
        }
    } else if (strcasecmp(compression_type, "zlib") == 0) {
        // Zlib
        uLongf estimated_size = len * 4;
        decompressed_data = (char *)malloc(estimated_size);
        if (decompressed_data == NULL) {
            return luaL_error(L, "内存分配失败");
        }

        int res;
        while (1) {
            res = uncompress((Bytef *)decompressed_data, &estimated_size, (const Bytef *)compressed_data, len);
            if (res == Z_OK) {
                decompressed_size = estimated_size;
                break;
            } else if (res == Z_BUF_ERROR) {
                estimated_size *= 2;
                char *new_buffer = (char *)realloc(decompressed_data, estimated_size);
                if (new_buffer == NULL) {
                    free(decompressed_data);
                    return luaL_error(L, "内存分配失败");
                }
                decompressed_data = new_buffer;
            } else {
                free(decompressed_data);
                return luaL_error(L, "Zlib解压失败");
            }
        }
    } else if (strcasecmp(compression_type, "zstd") == 0) {
        // Zstd
        unsigned long long estimated_size = ZSTD_getFrameContentSize(compressed_data, len);
        if (estimated_size == ZSTD_CONTENTSIZE_ERROR || estimated_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            return luaL_error(L, "无法获取Zstd解压后的长度");
        }

        decompressed_data = (char *)malloc(estimated_size);
        if (decompressed_data == NULL) {
            return luaL_error(L, "内存分配失败");
        }

        size_t res = ZSTD_decompress(decompressed_data, estimated_size, compressed_data, len);
        if (ZSTD_isError(res)) {
            free(decompressed_data);
            return luaL_error(L, "Zstd解压失败: %s", ZSTD_getErrorName(res));
        }
        decompressed_size = res;
    } else if (strcasecmp(compression_type, "none") == 0 || strcasecmp(compression_type, "no") == 0) {
        // 不解压
        decompressed_data = (char *)malloc(len);
        if (decompressed_data == NULL) {
            return luaL_error(L, "内存分配失败");
        }
        memcpy(decompressed_data, compressed_data, len);
        decompressed_size = len;
    } else {
        return luaL_error(L, "未知的解压类型: %s", compression_type);
    }

    struct reader rd;
    reader_init(&rd, decompressed_data, decompressed_size);

    int count = 0;
    while (rd.ptr < rd.len) {
        unpack_one(L, &rd);
        ++count;
    }

    free(decompressed_data);

    return count;
}


