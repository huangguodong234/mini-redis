# SDS

> 本文回答一个问题：**C 的 `char*` 有 5 个原生缺陷，怎么用一个字符串类型全解决？**
>
> 答案是 **header + 数据区**的内存布局 —— 数据前面紧贴一个头部存长度。
> 本文讲知识点（调用链见 `docs/字节流处理.md`）。

---

## 一、`char*` 的 5 个缺陷

```c
char *s = "hello";
```

| # | 缺陷 | 例子 |
|---|---|---|
| 1 | **取长度是 O(n)** | `strlen(s)` 要从头扫到 `\0` |
| 2 | **不是二进制安全** | 数据里有 `\0` → 后面被「切断」 |
| 3 | **容易缓冲区溢出** | `strcpy`/`strcat` 不检查目标容量 |
| 4 | **改字符串 = 频繁重分配** | N 次拼接 = O(N×len) + 碎片 |
| 5 | **不自描述** | 字符串不知道「我多大、还有多少余量」 |

> ★ 缺陷 2 最致命：协议里的值完全可能是二进制（图片、序列化数据），
> 用 `\0` 判断结束就会读错长度。

---

## 二、核心设计：header + 数据区

```text
        char *s 指向这里（数据区起点，也是 sds 的返回值）
              ↓
   ┌──────────────┬──────────────────────┬───┐
   │   sdshdr      │     字符数据区         │\0 │
   │ {len, alloc,  │   (真正的字符串内容)   │   │
   │   flags}      │                       │   │
   └──────────────┴──────────────────────┴───┘
     ↑ header         ↑ 从这里开始当普通 C 字符串用
```

**关键魔法**：`sds` 就是一个 `char*`，但它指向的数据区**前面**紧挨着头部。于是：

```c
size_t sdslen(const sds s) {
    /* s[-1] 是 flags，低 3 位是类型 → 按类型往回挪一个头部大小 */
    return sds_hdr8(s)->len;      /* O(1)，不扫描 */
}
```

- 头部地址 = `s - sizeof(header)`
- `sdslen()` 反查 header → **O(1)**
- `s` 同时是 `\0` 结尾的普通字符串 → `printf("%s", s)` 直接能用

> ★ 对外只是一个 `char*`，调用方不用传长度参数；
> 内部却能 O(1) 拿到长度和容量 —— **信息藏在指针身后。**

---

## 三、逐一解决那 5 个缺陷

| 缺陷 | SDS 的做法 |
|---|---|
| 1 长度 O(n) | `len` 存在 header，`sdslen()` 不扫描 |
| 2 非二进制安全 | 长度靠 `len` 记录，**不靠 `\0`**；数据里可有任意 `\0` |
| 3 缓冲区溢出 | 写前用 `sdsavail()` 判断，空间不足先扩容，绝不满时直写 |
| 4 频繁重分配 | 空间预分配 + 惰性释放（见下节） |
| 5 不自描述 | `len`/`alloc` 都在 header，扩容决策 O(1) |

---

## 四、减少内存重分配的两个策略

### 4.1 空间预分配 —— 扩容时「多给点」

```c
sds sdsMakeRoomFor(sds s, size_t addlen) {
    size_t avail = sdsavail(s);
    if (avail >= addlen) return s;        /* ★ 够用就原样返回，不碰内存 */

    size_t newlen = len + addlen;
    size_t newalloc;
    if (newlen < 1024 * 1024) {
        newalloc = newlen * 2 + 1;        /* < 1MB：按新长度翻倍 */
    } else {
        newalloc = newlen + 1024 * 1024;  /* ≥ 1MB：只多给 1MB */
    }
    ...
}
```

**效果**：连续 N 次小拼接，前几次都落在「已有多余空间」里，
不必每次 `realloc`。**均摊到每次 append 是 O(1)**（平摊分析）。

> ★ 1MB 阈值的作用：小字符串翻倍能省很多次分配；
> 大字符串若也翻倍，多出来的空间浪费太大（10MB 变 20MB），所以只加固定 1MB。

### 4.2 惰性释放 —— 清空时不还内存

```c
void sdsclear(sds s) {
    sds_hdr8(s)->len = 0;      /* ★ 只把 len 归 0，alloc 不变 */
    s[0] = '\0';
}
```

下次 append 直接复用这块空间，避免「用完就缩、缩完又扩」的抖动。

> ⚠️ **惰性释放 ≠ 内存池**，这两者很容易混：

| | 惰性释放（`sdsclear`） | 内存池（buffer pool） |
|---|---|---|
| 内存留给谁 | **同一个 sds 自己** | 池子里的**任意对象** |
| 解决什么 | 一次性的「扩→缩→再扩」抖动 | 跨对象反复 malloc/free |
| 本项目 | ✅ 用了（每个连接的 `querybuf`） | ❌ 没用 |

### 4.3 为什么本项目不用内存池

池化的收益建立在「**被换手的对象尺寸稳定、命中率高**」上。而本项目的
key/value/member 长度随机分布（`"OK"` vs 几百 KB 都可能）：

- 池里的块很难被「同尺寸」新对象命中 → **命中率趋近 0**
- 找不到合适块还是要 `malloc` → 等于每次多一次池子查找的白白开销

> ★ 而且 **glibc malloc 本身已按 size-class 分桶缓存空闲块**，
> 再做一层应用层池子收益极小。（真实 Redis 的 `zmalloc` 也没做通用缓存池。）

---

## 五、分规格头部（TYPE_5 / 8 / 16 / 32 / 64）

字符串长度差异巨大（`"OK"` vs 几 GB 缓存）。若都用固定 17B 头部，
存 3 字节的 `"abc"` 要占 18B，**浪费 5 倍**。

| 类型 | len/alloc 位宽 | 头部大小 | 适配数据长度 |
|---|---|---|---|
| `SDS_TYPE_5` | 无独立字段 | **1 B** | ≤ 31 字节 |
| `SDS_TYPE_8` | uint8 / uint8 | 3 B | < 256 字节 |
| `SDS_TYPE_16` | uint16 / uint16 | 5 B | < 64 KB |
| `SDS_TYPE_32` | uint32 / uint32 | 9 B | < 4 GB |
| `SDS_TYPE_64` | uint64 / uint64 | 17 B | 大字符串 |

```c
static inline char sdsReqType(size_t string_size) {
    if (string_size < 1 << 5)      return SDS_TYPE_5;    /* 32 */
    if (string_size < 1 << 8)      return SDS_TYPE_8;    /* 256 */
    if (string_size < 1 << 16)     return SDS_TYPE_16;
    if (string_size < 1ll << 32)   return SDS_TYPE_32;
    return SDS_TYPE_64;
}
```

> ★ 用 `<` 而不是 `<=`：长度正好等于 32/256 时落到**高一档**，
> 不会卡在 `TYPE_5` 放不下的边界上。

### TYPE_5 为什么特殊

它没有独立的 `len`/`alloc` 字段，**`len` 就存在 `flags` 的高 5 位里**：

```c
struct __attribute__((__packed__)) sdshdr5 {
    unsigned char flags;   /* 低 3 位 = type，高 5 位 = len */
};
```

- 上限 31（5 位能表示的范围）
- **所以 `alloc == len`，分配后不可增长** —— 要增长会先迁移到 `TYPE_8`

> ★ 这也是 `sdscatlen` 里要分 5 种类型分别更新 `len` 的原因：
> `TYPE_5` 走 `s[-1] = (len << 3) | TYPE_5`，其余走各自的头部字段。

---

## 六、为什么结尾总留一个 `\0`

即使数据本身是二进制（可能含 `\0`），末尾也**多写一个** `\0`：

- sds 可以**零拷贝**丢给任何按 C 字符串处理的库函数
  （`printf`、`strlen`、`snprintf`、文件读写）
- 既能当「干净的动态数组」（按 `len` 操作），又能当「C 字符串」（按 `\0` 操作）

> ★ 这就是 SDS **「字节数组 / 字符串两用」**的精髓。

---

## 七、为什么头部必须 packed

整个设计建立在这条换算上：

```c
flags 紧贴数据前 1 字节（s[-1]）
数据 = malloc 基址 + sizeof(header)
```

若头部**不 packed**，结构体会因字段对齐在尾部插入填充，
`sizeof` 就大于字段实际字节数：

- `s - sizeof(hdr)` **不再等于 malloc 基址** → `free()` 直接崩堆
- `flags` 也不在 `s[-1]` 了 → 类型判断全错

```c
struct __attribute__((__packed__)) sdshdr16 {
    uint16_t len;     /* 2 */
    uint16_t alloc;   /* 2 */
    unsigned char flags;  /* 1 */
};                    /* packed 后 sizeof = 5；不 packed 会是 6 */
```

> ⚠️ **本实现真的踩过这个坑**：`sdshdr16` 不 packed 时 `sizeof` 是 6 而非 5。

---

## 八、四个使用上的坑

| # | 坑 | 正确写法 |
|---|---|---|
| 1 | **改长度的函数可能搬家** | `s = sdscat(s, "x");` —— 必须用返回值接住 |
| 2 | 分配失败返回 NULL | 生产代码使用前判空 |
| 3 | `sdsfree` 后不能再用 | 悬垂指针 / 双重释放 |
| 4 | 不要手动越界写 | 走 API，否则绕过「先扩容再写」的保护 |

> ★ 坑 1 最常犯：`sdscat(s, "x")` 丢掉返回值 —— 旧 `s` 可能已被 `realloc` 释放。
> 本项目在 `send_response` 里就是这么处理的：
> ```c
> sds nb = sdscatlen(c->outbuf, data, len);
> if (!nb) { /* OOM，保留旧 sds */ return; }
> c->outbuf = nb;          /* ★ 成功才覆盖 */
> ```

---

## 九、一分钟速查

| 问题 | 答案 |
|---|---|
| `sds` 是什么 | 一个 `char*`，指向数据区，头部在它前面 |
| `sdslen` 为什么 O(1) | 反查头部读 `len`，不扫描 |
| 为什么二进制安全 | 长度靠 `len`，不靠 `\0` |
| 扩容策略 | < 1MB 翻倍；≥ 1MB 加 1MB |
| `sdsclear` 为什么不还内存 | 惰性释放，留给同一个 sds 下次复用 |
| 为什么不用内存池 | key/value 长度随机，命中率趋近 0 |
| 5 档头部怎么选 | 按长度选最小的那档（`sdsReqType`） |
| `TYPE_5` 特殊在哪 | `len` 存在 `flags` 高 5 位，上限 31，不可增长 |
| 为什么留 `\0` | 兼容所有按 C 字符串处理的库函数 |
| 头部为什么必须 packed | 否则 `s - sizeof(hdr)` ≠ `malloc` 基址，`free` 崩堆 |
| 相关代码 | `include/sds.h`、`src/sds.c`、`tests/test_sds.c` |
