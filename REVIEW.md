# mini-redis 项目审查报告

- **审查对象**：`D:\Code\cpp\mini-redis`（master，HEAD = `1ba4fce`）
- **审查日期**：2026-09-19
- **审查方式**：全量源码静态审查（9 个 `.h` + 10 个 `.c`，共 3045 行；另有 `tests/` 513 行）＋ 构建 / 测试 / 压测工程审查
- **重要说明**：当前环境为 Windows 且无 gcc / valgrind / redis-cli，**未做编译与运行验证**。所有结论均来自源码推理，标注「建议复现」的条目请在 Linux 上确认。

---

# 第二轮审查（2026-09-19，最新）

- **审查对象**：同上工作区，HEAD 仍为 `1ba4fce`，但工作区带 **16 个未提交修改 + 未跟踪 `tests/`、`REVIEW.md`**
- **审查范围**：10 个 `.c` + 9 个 `.h`（3045 行）＋ 4 个单测（513 行）＋ `test.sh`（450 行）＋ `Makefile` ＋ `bench/`
- **本轮方式**：全量源码复读 + **逐条核对第一轮结论与当前源码是否一致** + 补齐第一轮未覆盖的面
- **重要说明**：本机为 Windows / MSYS，**仍无 gcc / clang / tcc / valgrind / redis-cli**（`wsl.exe` 被本机安全策略拉黑，无法调用）。
  因此**本轮依然未编译、未运行、未做内存检测**。第一轮「实机验证结果（WSL）」小节里 42/42 与 valgrind 零泄漏的结论，
  **在当前环境无法复现验证**，只能作为上一轮会话的记录引用。

## 一、第一轮结论逐条复核

| 编号 | 第一轮结论 | 本轮复核（对照当前源码） | 判定 |
|---|---|---|---|
| P0-1 SIGPIPE | 已修 | `src/main.c:49-54` `sigaction(SIGPIPE, SIG_IGN)`；`src/server.c:100-101` 注明前提 | ✅ 属实 |
| P0-2 Ctrl+C | 已修（两处） | `src/server.c:270`（读循环头）+ `:275-282`（EINTR 分支）；`processInputBuffer` 内确无 `Keep_running`（`:336-341` 有说明） | ✅ 属实，与「更正 2」一致 |
| P0-3 单连接串行 | 待第 4 步 | `src/main.c:93-128` accept 后同步进入 `readQueryFromClient`；`src/server.c:270` 阻塞 read | ⏸ **仍在（当前唯一 P0）** |
| P0-4 OOM 静默丢数据 | 已修 | `hashtable_set:107-159`、`storage_set_steal:51-61`、`skiplist_add:74-149`、`zset_add:31-39` 均返回 `int`；`commands.c:110-114, 227-234` 回 `-ERR out of memory` 并 `return -1` | ✅ 属实（**协议层 OOM 仍误报，见 N-5**） |
| P1-1 `write()==0` | 降级为防御性加固 | `server.c:107-117` 注释措辞与「更正 1」完全一致 | ✅ 措辞纪律到位 |
| P1-2 `%g` 精度 | 已修 | `commands.c:43-54` `%.17g` + 裁尾零 | ✅ 属实（**大整数/`-0` 仍与 Redis 不同，见 N-4**） |
| P1-3 `long→int` 截断 | 已修 | `commands.c:176-180` `INT_MIN/INT_MAX` 校验 | ✅ 属实 |
| P1-4 扩容 `printf` | 已修 | `hashtable.c:63-65` 注释说明已移除；`:73` 改 `fprintf(stderr, ...)` | ✅ 属实 |
| P1-5 不检查 init | 已修 | `main.c:76-84` 判空 + `EXIT_FAILURE` | ✅ 属实 |
| P1-6 库函数 `exit()` | 未做 | `server.c:210, 226, 234` 三处 `exit(EXIT_FAILURE)` | ⏸ 仍在 |
| P1-7 无内存上限 | 未做 | `protocol.c:54,57` 单参数上限 512MB；`server.c:270-299` `querybuf` 无上限 | ⏸ **仍在（是 P0-3 的放大器）** |
| P1-8 README 过度宣称 | 已修 | README:7-16 边界声明 + 「与 Redis 的协议兼容性差距」小节 | ✅ 属实 |
| P1-9 `Client` 无空闲超时 | 未做 | `include/server.h:18-33` 仍无 `last_interaction`；但 `anet.c:92` 设了 TCP keepalive(60/10/3) | ⏸ 部分缓解（keepalive 只探死链，探不到「活着但一直不发数据」） |
| P2-1 `archive/` 重复 | 已修 | README:95-96 标注归档；新建 `tests/` | ✅ 属实 |
| P2-2 `distclean` / 尾随 Tab | 已修 | `Makefile:99-100` | ✅ 属实（**根目录 `final_bench.csv` 仍在，见 P2-8**） |
| P2-3 `-Wextra`/asan/依赖 | 已修 | `Makefile:17-19`、`:76-80` | ✅ 属实（**asan 与 unit 的 CFLAGS 不一致，见 N-6**） |
| P2-4 死字段 | 未做 | `argv_max` / `cmd_start` 只写不读（`server.c:77,80,405,420,428,473`） | ⏸ 仍在（**并新增 `sdsclear`/`sdsavail` 死 API，见 N-9**） |
| P2-5 `rand() & 1` | 未做 | `skiplist.c:13` | ⏸ 仍在 |
| P2-6 扩容失败静默 | 未做 | `hashtable.c:121` `(void)hashtable_resize(ht)` | ⏸ 仍在 |
| P2-7 陈旧 worktree | 已修 | `git worktree list` 只剩主工作区 | ✅ 属实 |
| P2-8 根目录残留 csv | 未做 | `final_bench.csv`（484B，09-16）仍在 | ⏸ 仍在 |
| P2-9 测试脚本优先级写法 | 未做 | `test.sh:187,190,193,212` 仍是 `[ A ] \|\| [ B ] && C \|\| D` | ⏸ 仍在（左结合语义实测正确，仅可读性差） |

**复核小结**：第一轮 24 条里 **14 条已真实落地**，10 条仍在（1 条 P0、4 条 P1、5 条 P2）。
第一轮报告本身**未发现"声称修了但代码没改"的情况**——结论与源码一致，这点值得肯定。

## 二、本轮新发现（第一轮未覆盖）

### N-1（P2 · 文档自相矛盾）README 仍宣称跳表命令是 O(log n)

- **位置**：`README.md:492` —— 「有序集合（ZADD/ZRANGE/ZREM/ZSCORE）基于跳表，**O(log n)**」
- **现象**：同一份 README 的 `:384` 和 `:478` 已经明确写着「`skiplist_find()` 是第 0 层线性扫描 **O(n)**，
  所以 ZADD/ZSCORE/ZREM **实际都是 O(n)**」。
- **根因**：第一轮修正了「历史性能数据」小节和「跳表 vs 哈希表」小节，但漏掉了「核心技术点 · 已实现」这一处同义表述。
- **影响**：同一文档两种说法并存，比单处写错更伤——读者会认为作者自己没想清楚。这是面试/评审场景下最容易被抓的点。
- **修复**：改为「基于跳表（当前 `skiplist_find` 为 O(n) 线性扫描，见下；待优化为真 O(log n)）」。

### N-2（P2 · 文档与代码不符）README 架构树漏掉 4 个源文件

- **位置**：`README.md:72-88`
- **现象**：`include/` 实际 **9 个** `.h`，树里只列 7 个（缺 `server.h`、`anet.h`）；`src/` 实际 **10 个** `.c`，树里只列 8 个（缺 `main.c`、`anet.c`）。
- **影响**：`main.c` 是程序入口，新人按图索骥找不到；`anet.c`（socket 选项封装）在整份 README 里**从未被提及**，属于"代码里有、文档里没有"的静默模块。
- **修复**：补齐这 4 个文件，并给 `anet.c` 补一句职责说明（SO_SNDBUF/SO_RCVBUF/keepalive/TCP_NODELAY/REUSEADDR 封装）。

### N-3（P2 · 文档与代码不符）README 写 40/40，`test.sh` 实际 42 个断言

- **位置**：`README.md:407`、`:533`、`:540`（三处 `40/40` / `35 → 40 用例`）
- **现象**：第一轮往 `test.sh` 新增了「RST 断连后服务器仍存活」「有连接时 SIGINT 能优雅退出」两个用例，
  用例数从 40 变成 **42**（本轮逐条点数确认：§0=1、§1=5、§2=3、§3=10、§4=4、§5=12、§6=2、§7=3、§8=2）。
  `REVIEW.md` 自己写的是 42/42 —— **两份文档互相打架**。
- **修复**：README 三处统一为 42；或按 `:211` 已有的措辞「以脚本输出的 `N/N 通过` 为准」改写，去掉硬编码数字。
- **附带**：`REVIEW.md` 开头写「9 个 `.h` + 10 个 `.c`，共 **3548** 行」，实际这 19 个文件是 **3045** 行（3548 是把 `tests/` 也算进去的数）。小误差，一并更正。

### N-4（P2 · 兼容性 / 数据格式）`format_score` 与 Redis `d2string` 在大整数与 `-0` 上仍不一致

- **位置**：`src/commands.c:43-54`
- **现象**（纯静态推导，**建议实机复现**）：
  - `ZADD z 1e17 m` → `ZSCORE z m` 本实现回 **`"1e+17"`**；真实 Redis 回 **`"100000000000000000"`**
    （Redis `d2string` 先试 `double2ll`，整数且能塞进 `long long` 就改用 `%lld`）。
  - `ZADD z -0 m` → 本实现回 **`"-0"`**；Redis 对 `value == 0` 有特判，回 **`"0"`**。
- **根因**：第一轮把 `%g` 换成 `%.17g` + 裁尾零，解决了 6 位有效数字截断（这是主要问题，修得对），
  但没有补上 Redis `d2string` 的另外两步（整数特判、零特判）。
- **影响**：**数据正确性层面的兼容性偏差**，非崩溃/内存问题。浮点分数 ≥1e16 或精确整数时文本形式与 Redis 不同；
  客户端若按字符串比对、落库或做哈希会不一致。
- **修复**：照抄 Redis `d2string` 的三步：① `value == 0` → `"0"`；② 试 `double2ll`
  （`value` 在 `LLONG_MIN..LLONG_MAX` 且 `(double)ll == value`）→ `%lld`；③ 否则 `%.17g` + 裁尾零。
  并在 README「与 Redis 的协议兼容性差距」表里补一行。

### N-5（P2 · 可观测性）协议层的 OOM 被报成「协议错误」

- **位置**：`src/protocol.c:75`（`extract_bulk_content` 内 `sdsnewlen` 失败 `return -1`）→ `src/server.c:362-365`
- **现象**：`extract_bulk_content` 把「内存分配失败」和「格式不合法」都编码成 `-1`。
  `processInputBuffer` 收到 `-1` 一律 `send_error(fd, "ERR protocol error")` 并断开，
  `server.c:304` 的日志也打「协议错误，断开连接」。
- **影响**：真出 OOM 时，**客户端收到的错误码和服务器日志都在说"你发的包格式错了"**，
  排障方向被完全带偏。第一轮 P0-4 修好了 SET/ZADD 路径的 OOM 上报，但**协议层这条 OOM 通路漏掉了**。
- **修复**：给 `extract_bulk_content` 增加区分返回码（如 `-2` = OOM），
  `processMultibulkBuffer` 据此回 `-ERR out of memory`，复用已有的 `-2 → 断开` 通道。

### N-6（P2 · 工程）`make asan` 与 `make unit` 的 CFLAGS 不一致，测试代码拿不到 sanitizer 覆盖

- **位置**：`Makefile:76-80`（asan）与 `:62-68`（unit）
- **现象**：`asan` 目标用递归 make，**只把 sanitizer 版 CFLAGS 传给了 `$(TARGET)`**。
  之后跑 `make unit`：`src/*.o` 已是最新（不会重编，仍是 asan 版），但 `tests/test_*.o` 会用**默认 CFLAGS** 编译。
- **后果**：
  1. 混合 `.o` 构建，链接能过但构建语义混乱，容易误判"这次测的是 sanitizer 版"；
  2. **测试代码自身**的栈/全局越界不会被检出 —— 第一轮恰恰是在 `test_sds.c` 里抓到过
     一个「用 4 字节数组去构造 40 字节 sds」的越界读（`REVIEW.md` 「更正 3」有记录）。
     如果那个 bug 还在，且 `tests/test_sds.o` 没被 asan 插桩，`make asan && make unit` 是**测不出来**的
     （全局/栈 redzone 需要该编译单元被插桩才会注册）。
- **边界说明（避免过度宣称）**：**泄漏检查仍然有效** —— LeakSanitizer 是进程级的，靠 malloc 拦截而非 TU 插桩。
  所以「make asan 后跑 make unit 验证零泄漏」成立；但「验证零越界/零 UB」这一半**不成立**。
- **修复**：加一个 `asan-unit`（或在 `asan` 里一并构建 `$(TEST_BINS)`），保证 sanitizer 版 CFLAGS 同时覆盖 `src/` 与 `tests/`。
  同步收窄 README/REVIEW 里「make asan 后跑 make unit 确认零泄漏、零 UB」的措辞。

### N-7（P2 · 脚本健壮性）`bench/bench.sh` 复现了 `test.sh` 已经踩过的三个坑

- **位置**：`bench/bench.sh:21, 30, 13/22`
- **(a) `/tmp` 固定名日志缺 UID 隔离**：`LOG="/tmp/bench-$PORT.log"`（`test.sh:22` 已改成带 `${UID}`）。
  别人的同名文件存在时，`"$BIN" "$PORT" >"$LOG" &` 在**重定向阶段**就失败（Permission denied）——
  进程根本没起来，`$!` 拿不到有效 PID，表现为「等待就绪循环空转 5 秒后继续，redis-benchmark 结果全空」。
  这正是 `test.sh` 已经修掉的那个**极具误导性**的症状。
- **(b) EXIT trap 无 `|| true`**：`trap "kill -9 $SRV 2>/dev/null" EXIT`。`kill -9` 对已 reap 的 PID 返回 1，
  会成为 trap 的退出码并可能覆盖显式 `exit N`。与 `test.sh:102` 已采用的 `|| true` 写法不一致。
- **(c) 注释与代码不符**：文件头说结果写入 `final_bench.csv`（`:13`），代码实际写 `bench_out.csv`（`:22`）。
- **另**：未检查 `redis-benchmark` 是否存在；缺失时 7 条用例全部静默产出空行，脚本仍报"结果已写入"。

### N-8（P2 · 文档语义）「10 并发」在本项目实为排队

- **位置**：`README.md:466-475`（历史性能数据 §3 标题「有序集合 / 跳表压测（10 并发，10 万请求）」）、`bench/bench.sh:55-56`（`-c 10`）
- **现象**：服务器是阻塞单连接串行，`-c 10` 只是把 10 条连接排在 `backlog` 里，
  并不存在"并发处理"。README 该小节虽已被 §9 用 O(n) 推翻了结论，但「并发」这层语义从未被点破。
- **修复**：在该小节标题下补一句「本项目为阻塞单连接串行，`-c 10` 实为排队，不是并发处理」。

### N-9（P2 · 死代码）除死字段外，还有两个无人调用的 SDS API

- **位置**：`src/sds.c:100`（`sdsclear`）、`src/sds.c:119`（`sdsavail`），声明在 `include/sds.h:303, 311`
- **现象**：全仓库（`src/` `tests/` `bench/` `archive/`）**零调用点**；
  且 `sdsMakeRoomFor` 内部是自己用 `sdsalloc_type` 算 `avail`（`sds.c:130`），并没有走 `sdsavail`。
- **附带**：`Client.cmd_start` 是 `int`，赋值来源 `c->qb_pos` 是 `size_t`（`server.c:405`，隐式窄化）；
  因为它从不被读取，目前无害，但属于同一处应清理的死代码。
- **修复**：删除，或在 `sds.h` 里标注「为 API 完整性保留（对齐 Redis sds 接口）」。

### N-10（P2 · 性能观察，非缺陷）分片直发的真实代价：`write()` 调用数 = 3N+1

- **位置**：`src/server.c:135-193`（`send_*` 分段）+ `src/commands.c:197-203`（ZRANGE 循环）
- **现象**：`ZRANGE key 0 -1` 返回 N 个成员时，数组头 1 次 + 每个成员 `send_bulk_string` 3 次（`$len\r\n` / 数据 / `\r\n`）
  → **共 3N+1 次 `write()` 系统调用**。N=1000 即 3001 次。
- **定性**：这是**有意的设计取舍**，不是 bug —— `include/commands.h:20-62` 已论证：阻塞式单线程模型下一次调用就能发完，
  不需要输出缓冲。但它确实是"分段直发"的**真实代价**，而现有文档只写了收益（"省内存、不用事件循环"），没写代价。
- **建议**：在 `commands.h` 那段设计说明里补一句代价；可选优化是用 `writev()` 或按元素累积到一个小缓冲，
  把 3N+1 降到 1（对 ZRANGE 大结果的延迟/吞吐有可观改善，也可能部分解释 README §9 里跳表命令吞吐偏低）。

## 三、风险提示：第一轮的全部修复都还没提交（建议优先处理）

- **证据**：`git status` —— HEAD 仍是 `1ba4fce`，工作区有 **16 个已修改文件**（`.gitignore`、`Makefile`、`README.md`、
  5 个 `.h`、9 个 `.c`、`test.sh`），加两个未跟踪项（`tests/`、`REVIEW.md`）。
- **风险**：`git checkout .` / `git stash` / `git clean -fd` / 磁盘或误操作都会**整体丢掉**这一轮修复，
  包括 SIGPIPE、Ctrl+C 优雅退出、OOM 上报、Makefile 工程化、整个 `tests/` 目录。
  也就是说：**当前仓库里"已修复"的状态只存在于工作区，不在任何 commit 里**。
- **建议**：立刻分组提交（建议 4 个 commit）：
  ① `Makefile` + `.gitignore`（构建工程化）；② `main.c` / `server.c`（P0-1 / P0-2 / P1-1）；
  ③ 存储与命令层（P0-4 / P1-2 / P1-3 / P1-4 / P1-5）；④ `tests/` + `test.sh` + `README.md` + `REVIEW.md`（验证与文档）。

## 四、第二轮结论

**一句话**：第一轮的止血与正确性修复**真实落地且质量良好**（24 条复核里 14 条属实、0 条虚报），
内存所有权纪律依然是全项目最强的一环；剩下的问题集中在**文档自洽性**（README 内部两处自相矛盾、与 `test.sh` 用例数不符）、
**架构层单点**（P0-3 阻塞单连接 + P1-7 无内存上限，二者叠加仍是"一个客户端即可让服务停摆"）、
以及**工程收尾**（asan 覆盖不完整、`bench.sh` 重蹈 `test.sh` 已修的坑、修复未提交）。

按第一轮的分级口径，**本轮没有发现新的 P0**；新发现的 10 条全部落在 P2，其中 N-4（分数文本格式）与
N-5（OOM 误报为协议错误）是唯二触碰"数据/语义正确性"的，建议优先于其余 P2 处理。

### 更新后的路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| 第 1–3 步 | 止血 / 正确性 / 工程化 | ✅ 已完成（第一轮，工作区未提交 —— 见 §三） |
| **第 3.5 步（半小时，零风险）** | **提交第一轮修复**（§三）；修 N-1 / N-2 / N-3 三处文档不一致 | ⏭ 建议立刻做 |
| **第 3.6 步（半天，低风险）** | N-5 OOM 误报为协议错误；N-4 `format_score` 对齐 Redis `d2string`；N-6 `asan` 覆盖 `tests/`；N-7 `bench.sh` 三处 | ⏭ 待办 |
| 第 4 步（1–2 周，架构） | `epoll` + 非阻塞 + 输出缓冲（P0-3）；`skiplist_find` 改真 O(log n) + 哈希索引 member→score；多 key 隔离；P1-7 三道闸（`proto-max-bulk-len` / 输出缓冲上限 / 全局内存上限） | ⏸ 待办（从"演示"到"可用"的分水岭） |
| 第 5 步（可选） | N-10 `writev` 合并响应；P1-6 去掉库层 `exit()`；P2-4/5/6/8/9、N-9 死代码清理 | ⏸ 待办 |

### 本轮必须在 Linux / WSL 上完成的验证清单

第一轮的验证清单（`make clean && make` / `make unit` / `./test.sh` / `make asan` + valgrind）**仍然有效且尚未在本环境复现**，此外本轮新增：

1. `make clean && make` —— 确认第一轮的 16 个改动文件**能编过**（这些改动从未在本机编译过）。
2. `./test.sh` —— 确认输出为 **42/42**（README 写的 40 是旧数）。
3. `make asan && make unit` 之后，**检查 `tests/*.o` 是否被 asan 插桩**（`nm tests/test_sds.o | grep -c __asan`）——
   预期为 0，即 N-6 描述的覆盖缺口确实存在；改成 `asan-unit` 后应 > 0。
4. `ZADD z 1e17 m` → `ZSCORE z m` —— 当前预期回 `1e+17`；对齐 Redis 后应回 `100000000000000000`（N-4）。
5. `ZADD z -0 m` → `ZSCORE z m` —— 当前预期回 `-0`；对齐后应回 `0`（N-4）。
6. 人为制造协议层 OOM（如 `ulimit -v` 限制后发大 bulk）—— 确认当前会回 `-ERR protocol error`（N-5 的现象）。

## 五、本轮审查局限

- 本机无编译器、无 valgrind、无 redis-cli，且 `wsl.exe` 被安全策略拦截 —— **本轮所有结论均为源码静态推理，未编译、未运行**。
- N-4 的 `%.17g` 输出（`1e+17` / `-0`）依赖 libc 的 `printf` 实现，**未实测**；不同 libc 可能给出不同文本，建议实机确认。
- N-6 关于「asan 未插桩测试 TU」的推断基于 Makefile 与 GNU make 的隐式规则语义，**未实测**；
  验证方法见上表第 3 条。
- 未审查 `archive/`（9 个陈旧 `.c`，已声明不参与构建）、`bench/*.py`（11 个压测脚本）的内部正确性，
  也未复核 `assets/` 与历史 commit。
- 第一轮「实机验证结果（WSL）」小节的 42/42 与 valgrind 零泄漏，**本轮无法复现验证**，仅作历史记录引用。

---

## 修复进度（2026-09-19 更新）

按路线图完成了**第 1–3 步**，第 4 步（epoll 架构）留待后续。所有改动均为静态审查后手工修改，
**本机无编译器，尚未编译验证**（验证清单见本节末尾）。

| 编号 | 问题 | 状态 | 落地位置 |
|---|---|---|---|
| P0-1 | 未处理 SIGPIPE，客户端 RST 断连可杀死进程 | ✅ 已修 | `main.c` 忽略 SIGPIPE；`server.c` 补 EPIPE 说明 |
| P0-2 | Ctrl+C 在有连接时无法退出 | ✅ 已修 | `server.c` **两处**：`read()` 的 EINTR 分支查标志 + 读循环头。~~解析循环头~~ **已撤回**（见下方更正说明） |
| P0-3 | 单连接独占服务器 | ⏸ 待第 4 步 | 需 epoll 改造 |
| P0-4 | OOM 静默丢数据却回 `+OK` | ✅ 已修 | `hashtable_set` / `storage_set_steal` / `skiplist_add` / `zset_add` 改为返回 `int`，失败时回 `-ERR out of memory` 并断开 |
| P1-1 | `write()` 返回 0 空转 | ⚠️ **降级为防御性加固**（原判为可复现 bug，是过度宣称） | `server.c` `send_response` 保留 `n <= 0` 分支，但注释已改写为"防御" |
| P1-2 | `ZSCORE` 用 `%g` 精度丢失 | ✅ 已修 | `commands.c` 新增 `format_score`（`%.17g` + 裁尾零，对齐 Redis `d2string`） |
| P1-3 | `ZRANGE` 的 `long→int` 截断 | ✅ 已修 | `commands.c` 加 `INT_MIN/INT_MAX` 校验 |
| P1-4 | 扩容 `printf` 打 stdout | ✅ 已修 | `hashtable.c` 移除；失败信息改 stderr |
| P1-5 | 不检查 `storage_init` / `zset_create` | ✅ 已修 | `main.c` 判空后 `return EXIT_FAILURE` |
| P1-6 | 库函数里直接 `exit()` | ⏸ 未做 | 不在第 1–3 步范围 |
| P1-7 | 无内存上限（低成本 DoS） | ⏸ 未做 | 需先定阈值与超限行为，建议随第 4 步一起决策 |
| P1-8 | README「生产级健壮性」等过度宣称 | ✅ 已修 | README 首部加边界说明、§6 表格改措辞、新增「与 Redis 的协议兼容性差距」小节 |
| P1-9 | `Client` 无空闲超时 | ⏸ 未做 | 需 `last_interaction` + 定时器，随第 4 步 |
| P2-1 | `archive/` 重复代码 + 架构图不符 | ✅ 已修 | 新建 `tests/`；README 标注 `archive/` 为历史归档、不参与构建 |
| P2-2 | Makefile `distclean` 陈旧目标 / 尾随 Tab | ✅ 已修 | Makefile 重写 |
| P2-3 | 缺 `-Wextra` / asan / 头文件依赖 | ✅ 已修 | Makefile 加 `-Wextra`、`-MMD -MP`、`asan` 目标 |
| P2-7 | 陈旧 git worktree | ✅ 已修 | `git worktree prune` |
| P2-4/5/6/8/9 | 死字段、`rand()&1`、扩容失败静默、残留 csv、测试脚本优先级写法 | ⏸ 未做 | 均为可选优化，不影响正确性 |

**新增能力**：`tests/` 下 4 个断言式单元测试（`make unit` / `make check`），覆盖 SDS、哈希表、
跳表、有序集合；`test.sh` 新增断连（SIGPIPE 回归）、优雅退出（SIGINT 回归）、并发能力（记录项）三类用例。

### ⚠️ 必须在 Linux 上完成的验证清单

1. `make clean && make` —— 确认 `-Wextra` 下无新增告警、无编译错误。
2. `make unit` —— 4 个单元测试全绿（尤其 `test_skiplist` 的 500 成员随机压力与排序校验）。
3. `./test.sh` —— 集成回归全绿；重点看新增的「RST 断连后服务器仍存活」与「有连接时 SIGINT 能优雅退出」两项。
4. `make asan` 后跑 `make unit` 与 `SKIP_BUILD=1 ./test.sh` —— 确认零泄漏、零 UB（README 声称的「零泄漏」此前从未自动化验证）。
5. 手动确认 `ZSCORE` 精度：`ZADD z 1234567890 m` 后 `ZSCORE z m` 应回 `1234567890`（修复前是 `1.23457e+09`）。
6. 确认移除扩容 `printf` 后，压测日志不再被 `[扩容]` 刷屏。

### 两处结论更正（经用户 review 后修正）

**更正 1：P1-1 从「可复现 bug」降级为「防御性加固」。**

原报告称 `write()` 返回 0 会让 `send_response` 永久空转卡死。这个判断**不成立**：本项目用的是
**阻塞 socket**，发送缓冲区写满时 `write()` 会阻塞等待，不会返回 0；非阻塞 socket 则返回 `-1/EAGAIN`。
按 POSIX，`count > 0` 时成功返回的是「实际写入的字节数」，不会是 0。所以 `n == 0` 在当前场景**不可达**。

保留该分支仍然合理（真遇到 0 时 `continue` 会变成 100% CPU 空转，`break` 只是放弃本次响应），
但它是**防御性写法**，不应被列为「修复了一个 bug」。`src/server.c` 的注释已同步改写。

**更正 2：P0-2 的修复点从「三处」收回为「两处」。**

原实现额外在 `processInputBuffer` 的解析循环条件里也加了 `Keep_running`，这是**多余**的，而且引入了副作用：
SIGINT 到来时解析循环会中途跳出，缓冲区里**已经收下但还没执行的命令被无谓丢弃**
（内存上不泄漏——`sdsrange` 会把未处理数据保留在缓冲区，随后由 `free_client` 统一释放，
但语义上就是「把已经收下的活丢了」）。

正确的分工是：

- `readQueryFromClient` 的 `read()` **EINTR 分支**查标志 —— 覆盖「阻塞在 `read()` 上时收到 SIGINT」；
- `readQueryFromClient` 的**读循环头**查标志 —— 覆盖「处理命令期间 SIGINT 已被投递并消费，之后再阻塞进 `read()` 就等不到第二次 EINTR」；
- **解析层不参与** —— `processInputBuffer` 的契约是「把缓冲区里所有完整命令一次处理完」，不该背「感知关停」这个职责。

读循环头那次检查只会落在本轮 `processInputBuffer` 把缓冲区**排空之后**，因此不会丢命令。
`src/server.c` 已回退该处改动并补充说明。

**更正 3：第 3 步的 Makefile 静态模式规则写错，已在首次实机构建时暴露并修复。**

`$(TEST_BINS): %: tests/%.o $(COMMON_OBJS)` 里的目标模式写成 `%`，会让 stem 取到整个 `tests/test_sds`，
前置展开成 `tests/tests/test_sds.o`，于是 `make unit` / `make check` 直接报
`No rule to make target 'tests/tests/test_sds.o'`。已改为 `tests/%`（stem 只取 `test_sds`）。

同一轮还修了：

- `valgrind` 目标原先直接调用，未安装 valgrind 时只得到 `No such file or directory`（退出码 127），
  分不清是"没装"还是"跑挂了"。已加 `command -v` 探测 + 可操作的安装提示。
- `asan` 目标补一句提示：sanitizer 版与普通版的 `.o` 不通用，恢复普通构建需 `make clean && make`。
- 另修两个自写测试的缺陷：`test_sds.c` 拿 4 字节数组去构造最长 40 字节的 sds（栈上越界读，ASan 会直接报）；
  `test_skiplist.c` 用 9999 当"最大分数"，而随机成员的 score 最大约 `99999/7 ≈ 14285`，断言会偶发失败，已改用 `1e9`。

> 这三处都是"纸面审查 + 无编译器"必然留下的盲区。后续已在 WSL 实机验证并全部修复，
> 详见下方「实机验证结果」小节。

### 实机验证结果（WSL / Ubuntu，已通过）

在 WSL 上完成编译与全量运行后，第 1–3 步的状态确认为**已完成并验证**：

| 验证项 | 结果 |
|---|---|
| `make clean && make`（`-Wextra`） | 通过，无阻断性告警 |
| `make unit`（4 个断言式单测） | 全绿，含 `test_skiplist` 的 500 成员随机压力与排序校验 |
| `./test.sh` 集成回归 | **42/42 通过**，含新增的断连（SIGPIPE 回归）与优雅退出（SIGINT 回归）两项 |
| valgrind | `440 allocs / 440 frees`，**All heap blocks were freed — no leaks are possible** |

配套修掉的三个"纸面审查看不出来"的构建 / 测试环境问题：

1. **Makefile 静态模式规则**：目标模式 `%` → `tests/%`（否则前置展开成 `tests/tests/test_sds.o`）。
2. **bash EXIT trap 覆盖退出码**：trap 末尾的 `wait $SERVER_PID` 会把显式 `exit N` 改成别的值。
   改为 `kill ... || true`；并用 `pkill -x -u $UID mini-redis` 清理上次残留的服务进程（端口冲突的根源）。
3. **`/tmp` 日志撞名**：固定名日志若已被别的用户 / root 创建，当前用户既不能 `>` 覆盖、
   也因 sticky bit 不能 `rm`。日志名加 `${UID}` 隔离。

## 一、总体评价

| 维度 | 评价 | 说明 |
|---|---|---|
| 数据结构实现 | ★★★★☆ | SDS 五档 header、哈希表、跳表全部手写且基本正确，边界处理有意识 |
| 内存所有权纪律 | ★★★★☆ | steal / dup 两条路径边界清晰、注释详尽，是全项目最亮的部分 |
| 网络与协议 | ★★★☆☆ | RESP 粘包/半包状态机设计正确，但缺 SIGPIPE、超时、并发等生产必备件 |
| 健壮性与安全 | ★★☆☆☆ | 存在可被单个客户端直接搞挂 / 搞死的缺陷；无认证、无内存上限 |
| 工程与文档 | ★★★★☆ | README 质量远超一般练手项目，但有若干与代码不一致处 |
| 测试覆盖 | ★★★☆☆ | 40 个集成用例设计良好，但缺并发 / 断连 / 大表 / 单元测试 |

**一句话结论**：这是一份数据结构功底扎实、内存管理纪律良好、文档质量罕见的 C 语言练手项目；但它目前是「能正确跑通演示场景」而非「能对外提供服务」—— 存在一个能让进程直接被杀死的 P0 缺陷，以及若干数据正确性与协议兼容性偏差。

---

## 二、值得肯定的亮点

1. **SDS 五档 header 实现完整**，`(unsigned char)` 位移修复（`src/sds.c:16`）是真踩过坑并解决了的证据，README 也把坑记录了下来。
2. **半包/粘包状态机正确**：`multibulklen` / `bulklen` / `qb_pos` 三变量存档 + `sdsrange` 压缩，逻辑与 Redis 一致。`extract_bulk_content` 用 `buf_end` 做边界判断而不是靠 `'\0'`（`src/protocol.c:72`）—— 这是正确且必要的做法。
3. **内存所有权契约被写成文档并落到注释里**（`storage.h:20-31`、`zset.h:16-22`、`skiplist.h:54-57`）。「每次 steal 必须成对置 NULL」这条硬契约被识别并写明，是成熟度的表现，而不是"碰巧写对了"。
4. **分段直发 send 层**（`src/server.c:114-184`）避免了不定长固定数组；`send_null_bulk` 把 `"$-1\r\n"` 的长度从 6 修成 5（`src/server.c:175-177`），说明对二进制精确性有要求。
5. **README 的压测方法论**（四把尺子 + 交错 A/B + 中位数 + 诚实标注 VM 噪声 ±30%）水平高于绝大多数同类项目。对 `skiplist_find` 是 O(n) 的自我发现也很诚实。

---

## 三、问题清单

### P0 —— 必须修复（可导致服务不可用或数据错误）

#### P0-1 未处理 SIGPIPE，客户端异常断开可直接杀死服务器进程

- **位置**：`src/server.c:100-112`（`send_response`）、`src/main.c:34-42`（仅注册 SIGINT）
- **现象**：`send_response` 调用 `write()`；全项目没有任何 `signal(SIGPIPE, SIG_IGN)`，也没用 `send(..., MSG_NOSIGNAL)`。
- **根因**：POSIX 规定向已收到 RST 的 socket 写入会先投递 SIGPIPE，**默认动作是终止进程**。
- **触发**：客户端发完命令不读回复就关闭（内核回 RST）、带 `SO_LINGER(1,0)` 强制 RST 关闭、或 `GET` 大值后立刻 close。此后服务器任何 `write()` 都会拿到 EPIPE + SIGPIPE。
- **影响**：**整个进程死亡**，且发生在 `free_client` 之前 —— 所有连接的数据结构全部丢失。这不是"连接级错误"，是"服务下线"。
- **修复**：

```c
/* main.c 的 setup_signal_handler 内 */
signal(SIGPIPE, SIG_IGN);
```

  并把 `send_response` 中 `errno == EPIPE` 视为正常断开（静默 return）。
- **复现建议**：Linux 上用 Python 客户端 `setsockopt(SOL_SOCKET, SO_LINGER, struct.pack('ii',1,0))` 后 `close()`，紧接一条 `GET` 大值命令。

#### P0-2 Ctrl+C 在有空闲客户端连接时无法退出服务器

- **位置**：`src/server.c:254-266`
- **现象**：读循环里 `if (errno == EINTR) { continue; }`，**从不检查 `Keep_running`**。
- **根因**：SIGINT 处理函数只置 `Keep_running = 0`；`read()` 被打断返回 EINTR 后代码直接 `continue`，重新阻塞在 `read()`。讽刺的是 `sa_flags = 0`（未设 `SA_RESTART`）本意就是让系统调用被打断，结果被打断后又被忽略。
- **影响**：README 宣称的「SIGINT 信号处理，优雅退出」实际**只在没有客户端连接时成立**。只要有任意一个客户端连着（哪怕完全空闲），Ctrl+C 就毫无响应，必须等客户端断开。这是用户必然会遇到的体验问题。
- **修复**：

```c
if (errno == EINTR) {
    if (!Keep_running) break;   /* 补上这一句 */
    continue;
}
```

  更彻底的做法是用 `poll()` 带超时读，让循环周期性检查退出标志。

#### P0-3 阻塞式单连接串行：一个连接独占整个服务器

- **位置**：`src/main.c:70-105`、`src/server.c:242-296`
- **现象**：`accept` 后直接进入 `readQueryFromClient` 的阻塞循环，处理完才回到 `accept`。
- **影响**：
  - 同时只能服务一个客户端；第 2 个客户端的连接停在 backlog 里排队，而不是被并发处理或明确拒绝。
  - **一个慢客户端（telnet 挂住不发数据、或只发半条命令）会让整个服务对所有人不可用**。这是可用性缺陷，不只是性能问题。
  - 压测里所谓的"10 并发"实际上只是排队，README「历史性能数据」中 10 并发那组数据的语义需要重新审视。
- **修复方向**：引入 `epoll` 事件循环 + 非阻塞 socket + 每连接输出缓冲。README「待实现」已列出，但需要认识到它是**从"演示"到"可用"的分水岭**，优先级应高于 AOF 持久化。

#### P0-4 OOM 时静默丢数据，却向客户端回复 `+OK`

- **位置**：`src/storage.c:51-55`、`src/hashtable.c:130-136`、`src/commands.c:180-183`
- **现象**：`storage_set_steal` 返回 `void`；`hashtable_set` 在 `malloc(HashNode)` 失败时执行 `sdsfree(key); sdsfree(value); return;` —— 即**丢弃数据**。命令层无从感知，照常 `send_simple_string(fd, "OK")`。
- **影响**：内存不足时客户端收到 `OK` 但数据不存在 → **静默数据丢失**，比明确报错严重得多。`zset_add` → `skiplist_add` 存在同构问题（`src/skiplist.c:119-123`、`128-133`）。
- **修复**：让 `hashtable_set` / `storage_set_steal` / `zset_add` / `skiplist_add` 返回 `int`（成功 / 失败），命令层据此回 `-ERR out of memory` 或按既有约定断开（走 `-1` → `-2` 路径）。

---

### P1 —— 应当修复

| 编号 | 问题 | 位置 | 说明与修复 |
|---|---|---|---|
| **P1-1** | `send_response` 对 `write()` 返回 0 的处理缺失（**防御性**） | `src/server.c:103-111` | 原先只判 `n < 0`，`n == 0` 时会 `p += 0; len -= 0` 导致空转。**更正**：阻塞 socket 上 `n == 0` 实际不可达（缓冲区满时阻塞等待，非阻塞则返回 `-1/EAGAIN`；按 POSIX，`count > 0` 的成功返回是"实际写入字节数"，不会是 0），所以这是**防御性加固**，不是可复现的 bug。仍保留该分支的理由是代价不对称：真遇到 0 时 `continue` 会变成 100% CPU 死循环，`break` 只是放弃本次响应 |
| **P1-2** | `ZSCORE` 用 `%g` 格式化分数，精度丢失 + 输出科学计数法 | `src/commands.c:114` | `%g` 默认只有 **6 位有效数字**。score=1234567890 返回 `"1.23457e+09"`（Redis 返回 `"1234567890"`）；score=1.23456789 返回 `"1.23457"`。这是**数据正确性**问题。改用 `%.17g`（可无损往返 double） |
| **P1-3** | `ZRANGE` 的 `long → int` 截断未校验 | `src/commands.c:132-139` | `parse_strict_long` 只保证在 `long` 范围内，随后直接 `(int)start_l`。`ZRANGE k 4294967297 -1` 中 `(int)4294967297 == 1`，返回完全不相干的结果。应校验落在 `INT_MIN..INT_MAX` |
| **P1-4** | 哈希表扩容用 `printf` 打到 stdout | `src/hashtable.c:63-65` | 库层代码直接写 stdout。压测/生产时（stdout 重定向）每次扩容产生同步 IO，日志被 `[扩容]` 淹没，也干扰 `test.sh` 的日志分析。应删除或改 `fprintf(stderr, ...)` 并加开关 |
| **P1-5** | `main` 不检查初始化返回值 | `src/main.c:60-61` | `storage_init()` / `zset_create()` 失败返回 NULL 却被直接使用，随后 `storage_set_steal(NULL, ...)` 静默返回、客户端仍收到 `OK`（与 P0-4 叠加成"全程静默失败"） |
| **P1-6** | 库函数里直接 `exit()` | `src/server.c:193-230` | `create_server_socket` 三处 `exit(EXIT_FAILURE)`。库函数不应终止宿主进程，导致无法单测、无法优雅降级。改为返回错误码，由 `main` 决定退出 |
| **P1-7** | 无单连接 / 全局资源上限，低成本 DoS | `src/protocol.c:57`、`src/server.c:271`、`src/server.c:394` | 单参数上限 512MB，最多 1024 个参数，且 `querybuf` 可无限增长。配合 P0-3 的单连接独占，**一个客户端即可耗尽 2GB 内存并让服务停摆**。Redis 有 `proto-max-bulk-len` / `client-output-buffer-limit` / `maxmemory` 三道闸，本项目一道都没有 |
| **P1-8** | 无认证 + 监听 `INADDR_ANY`，README 宣称过度 | `src/server.c:211`、`README.md` §6 | 默认监听 `0.0.0.0:6379` 且无 AUTH，任何能访问该端口者可读写全部数据。练手项目可接受，但 README §6 把 SDS 版标为「生产级健壮性 ✅ 可用」是**过度宣称** —— 缺认证、内存上限、超时、并发、持久化中的任何一项都谈不上生产可用。建议改为"相对无 SDS 版在数据正确性与容量上更健壮" |
| **P1-9** | `Client` 无空闲超时 | `include/server.h:11-26` | `Client` 没有 `last_interaction` 字段。配合 P0-2 / P0-3，一个 TCP 连接只要不关就永久占住服务器。建议增加空闲超时（对应 Redis 的 `timeout` 配置） |

---

### P2 —— 工程质量与一致性

| 编号 | 问题 | 位置 | 建议 |
|---|---|---|---|
| P2-1 | `archive/` 与 `src/` 存在重复 / 陈旧代码（`main.c`、`zset.c`、5 个 `test_*.c`），且 README 架构图仍写 `src/test_resize.c` 等，与实际路径不符 | `README.md:78-80`、`archive/` | 删除 archive（git 历史已保留）或在 README 中注明其定位 |
| P2-2 | Makefile 的 `distclean` 引用了已移入 archive 的目标（`test_client` / `test_hash` 等）；`clean` 行尾有尾随 Tab | `Makefile:45,49` | 清理规则、去掉尾随 Tab |
| P2-3 | 编译选项只有 `-Wall`，缺 `-Wextra -Wpedantic`；无 `-fsanitize=address,undefined` 目标；无头文件依赖生成，改 `.h` 不会触发重编 | `Makefile:10` | 加 `-Wextra`；加 `asan` 目标；用 `-MMD -MP` 生成 `.d` 依赖 |
| P2-4 | 死字段：`Client.argv_max`、`Client.cmd_start` 定义并维护但从未被读取 | `include/server.h:18,25` | 删除或补齐用途 |
| P2-5 | `random_level()` 用 `rand() & 1` 取低位 | `src/skiplist.c:13` | LCG 低位随机性差，层数分布会偏离几何分布，影响跳表高度均衡。改用高位（`rand() < RAND_MAX / 2`）或更好的 PRNG |
| P2-6 | 哈希表扩容失败被静默忽略，负载因子继续恶化 | `src/hashtable.c:68-72` | 至少记录错误。功能仍正确，但会退化为长链 O(n) 查找 |
| P2-7 | 陈旧 git worktree 引用 `/tmp/nosds-wt`（已 prunable） | `git worktree list` | 执行 `git worktree prune` |
| P2-8 | 仓库根残留 `final_bench.csv`（被 `.gitignore` 的 `*.csv` 覆盖但文件仍在工作区） | 根目录 | 移入 `bench/` 或删除 |
| P2-9 | `[ A ] \|\| [ B ] && C \|\| D` 的写法依赖 shell 左结合，可读性差、易改错 | `test.sh:176,179,182,201` | 改用 `if ... then ... else` |

---

## 四、与真实 Redis 的协议兼容性差距

README 首句宣称「兼容 Redis 协议」。按当前实现，准确说法应是「**实现了 RESP 编码，并支持 8 个命令的子集**」。逐条差距：

| 命令 | 当前行为 | Redis 行为 | 严重度 |
|---|---|---|---|
| `ZADD` | 恒回 `+OK` | 回 `:1`（新增）/ `:0`（更新） | 中：`redis-cli` 可读，但客户端脚本判分会出错 |
| `ZADD` | **`key` 被 `(void)key` 忽略，所有有序集合共享全局单例** | 按 key 隔离 | **高** |
| `ZADD` | 不支持 `NX / XX / GT / LT / CH / INCR` | 支持 | 中 |
| `ZRANGE` | 不支持 `WITHSCORES` / `REV` / `BYSCORE` | 支持 | 中 |
| `PING` | 无论参数个数恒回 `+PONG` | `PING msg` 回 `$<len>\r\nmsg` | 中 |
| `SET` | 不支持 `EX / PX / NX / XX / GET` | 支持 | 低（README 已提） |
| `*0\r\n` | `parse_multibulk_header` 中 `n <= 0` → 协议错误并**断开连接** | 视作空命令，不断开 | 低 |
| `DEL` | 只支持单 key | 支持多 key | 低 |

**其中「`ZADD` 忽略 key」值得单独强调**：README 的 ZADD 示例用了 `myzset`，但实现里 `myzset` 完全没参与任何逻辑（`src/commands.c:60-61, 91-92, 105-106, 128-129` 四处 `(void)key`）。用户写 `ZADD a 1 x` 和 `ZADD b 2 y` 后，`ZRANGE a 0 -1` 会同时看到 `x` 和 `y`。这是**功能性缺陷**，不只是兼容性偏差。建议要么在 README 显著位置声明「有序集合为全局单例，key 参数当前被忽略」，要么尽快补上多 key 字典。

另外注意 `test.sh:221` 的 `12-arg PING` 用例把「PING 带参数仍回 PONG」这一**不兼容行为固化成了期望值** —— 未来若修正 `PING` 语义，这个测试会反过来报错。

---

## 五、测试覆盖评估

**优点**：40 个用例覆盖了协议层最难的部分（粘包 / 半包分帧、inline 命令、二进制 `\0`、超大值），`raw_send_split` 用真实 TCP 拆分来模拟半包的做法很扎实，`py_roundtrip` 做二进制精确比对也很到位。

**缺口**（按优先级）：

1. **无断连 / 异常路径测试** —— 正是 P0-1 能长期潜伏的原因。应加「发完命令立刻 RST 关闭」的用例。
2. **无优雅退出测试** —— P0-2 未被发现的原因。应加「保持连接状态下发 SIGINT，断言进程退出」。
3. **无并发测试** —— P0-3 未被暴露。
4. **`archive/test_skiplist.c`、`test_resize.c`、`test_hashtable.c` 未接入构建** —— 跳表在**大量随机 member** 下的排序正确性、哈希表扩容后的数据完整性，当前没有任何自动化覆盖。`test.sh` 里 zset 最多只有 4 个成员，跳表的多层结构几乎没被压到。建议把这些单测接回 Makefile 的 `test` 目标。
5. **无边界负向测试** —— 512MB 参数上限、`*1025`、`$-1`、`$abc`、超长 inline 行等。
6. **无泄漏回归** —— README 提到 valgrind / ASAN，但 Makefile 里没有对应目标。建议加 `make asan && ./test.sh`。

---

## 六、压测方法论复核

- 方法论本身（四把尺子 + 交错 A/B + 中位数 + 诚实标注 VM 噪声 ±30%）是**合格且高于平均水平**的。
- **需注意**：`bench/bench_focus.py` 的 `REQS = 30000` 单轮、无重复，而 README 自己也承认单跑波动 ±30%。因此 README 表格中未做 A/B 的数字（如「SET 54,674 vs 136,534」）**只能看量级，不能看倍数**。建议在所有对比表上统一标注测量轮数，或统一改用 `bench_ab.py`。
- **存在自相矛盾**：README「历史性能数据」小节写「跳表 ZADD（2384 QPS）与哈希表 SET（2535 QPS）差距仅约 6%，O(log n) 在工程实践中几乎接近 O(1)」，与后文第 9 节源码实测的「`skiplist_find` 是 O(n) 线性扫描」**直接冲突**。第 9 节的结论是对的，建议修正或删除历史小节中「O(log n) 接近 O(1)」的表述，否则会误导读者。

---

## 七、修复路线图建议

| 阶段 | 内容 | 收益 |
|---|---|---|
| **第 1 步（半天，止血）** | P0-1 SIGPIPE、P0-2 Ctrl+C、P1-1 `write()==0`、P1-2 `%.17g` | ✅ **已完成**（另顺手修了 P1-4）。其中 P1-1 事后更正为防御性加固，真正止血的是 P0-1 / P0-2 / P1-2 三条 |
| **第 2 步（1 天，正确性）** | P0-4 存储层返回错误码；P1-3 索引范围校验；P1-5 检查 init；P1-8 修正 README 宣称；处理「ZADD 忽略 key」 | ✅ **已完成**（ZADD key 改用文档明确说明，多 key 隔离留到第 4 步） |
| **第 3 步（2-3 天，工程化）** | Makefile 加 `-Wextra` / `asan` / 头文件依赖；补单测；补断连 / 退出 / 并发用例；`git worktree prune` | ✅ **已完成并实机验证**（42/42 集成 + 4 个单测全绿 + valgrind 零泄漏）。单测放在新目录 `tests/` 而非复活 archive —— 后者是旧 API 的打印式 demo，直接编译会崩 |
| **第 4 步（1-2 周，架构）** | `epoll` + 非阻塞 + 输出缓冲（P0-3）；`skiplist_find` 改真 O(log n) + 哈希索引 member→score；多 key 隔离 | ⏸ **待办**：这是从"演示"走向"可用"的分水岭 |

---

## 八、审查局限

- 本机为 Windows 且无 gcc / valgrind / redis-cli，**未编译、未运行、未做内存检测**。README 声称的「valgrind 零内存泄漏」「ASAN + LeakSanitizer 全路径零泄漏」**未经验证**。
- P0-1 的触发依赖内核 RST 时序，建议在 Linux 上用带 `SO_LINGER(1,0)` 的 Python 客户端复现确认。
- 压测数据的可信度评估基于脚本静态阅读，未实测。
- 未审查 `assets/`、`.vscode/` 与历史 commit 的代码质量。
