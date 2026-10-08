# ============================================================
# Makefile for mini-redis
# 用法：
#   make          → 编译生成 mini-redis
#   make test     → 编译 + 跑集成回归（./test.sh）
#   make unit     → 编译 + 跑 tests/ 下的断言式单元测试
#   make check    → unit + test（完整验证）
#   make asan     → 用 ASan/UBSan 重新编译（内存与未定义行为检测）
#   make valgrind → valgrind 跑内存泄漏检查
#   make clean    → 删除 .o / .d / 可执行文件 / 测试程序
# ============================================================

# 1. 定义变量：以后换编译器或加参数只需改这里
CC      = gcc
# -Wall 显示常见告警；-Wextra 补齐 -Wall 未覆盖的一类（未用参数、符号/无符号比较等）。
# 注：不加 -Werror —— 告警需要被看见，但不该在环境差异上把构建直接打断。
CFLAGS  = -Wall -Wextra -O2 -g -Iinclude
# 自动生成头文件依赖：改了 .h 会触发对应 .c 重编（原先改头文件不会重编，是隐患）
DEPFLAGS = -MMD -MP
RUN   = 200000
REPS  = 2000000
TARGET  = mini-redis

# 2. 源文件列表
SRCS = src/main.c src/server.c src/networking.c src/protocol.c src/commands.c src/storage.c src/sds.c src/anet.c src/dict.c src/skiplist.c src/zset.c src/ae.c

# 3. 将 .c 后缀替换为 .o；.d 是自动生成的依赖文件
OBJS    = $(SRCS:.c=.o)
DEPS    = $(OBJS:.o=.d)

# 4. 单元测试：tests/ 下每个 .c 自带 main()，链接通用模块。
#    注意 COMMON_OBJS 里【不含】src/main.o，否则会撞 main 符号。
#    多余的 .o 不会被链接进来，不影响产物。
TEST_SRCS   = tests/test_sds.c tests/test_dict.c tests/test_skiplist.c tests/test_zset.c tests/test_skipspan.c
TEST_BINS   = $(TEST_SRCS:.c=)
TEST_DEPS   = $(TEST_SRCS:.c=.d)
COMMON_OBJS = src/sds.o src/dict.o src/skiplist.o src/zset.o

# ------------------------------------------------------------
# 默认目标
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# 5. 模式规则：如何从 .c 生成 .o（顺带产出 .d 依赖文件）
%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# 单元测试可执行（静态模式规则）
# ⚠ 目标模式必须写 tests/% ，不能只写 % ——
#   静态模式规则会用"目标模式"去匹配目标名，匹配后剩下的部分才是 stem。
#   目标 tests/test_sds 配 %          → stem = "tests/test_sds" → 前置 tests/tests/test_sds.o ✗
#   目标 tests/test_sds 配 tests/%    → stem = "test_sds"       → 前置 tests/test_sds.o       ✓
$(TEST_BINS): tests/%: tests/%.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# ⚠ tests/test_lookup_order 是【白盒】测试：它 #include "../src/dict.c"，
#   为了直接对拍内部函数 dictFindEntry（static inline，不导出）。
#   因此它【不能】链 src/dict.o —— 会与包含进来的定义重复冲突。
#   所以它不进 TEST_SRCS，单独给一条只链 src/sds.o 的规则。
#   目标名【不能】叫 tests/test_lookup_order（会被上面的 tests/% 规则匹配）。
test_lookup_order: tests/test_lookup_order.c src/dict.c src/sds.c include/dict.h include/sds.h
	$(CC) $(CFLAGS) -o tests/test_lookup_order tests/test_lookup_order.c src/sds.c

# ⚠ test_lookup_order 是【白盒】测试：它 #include "../src/dict.c" 以直接对拍
#   内部函数 dictFindEntry（static inline，不导出）。因此它【不能】再链接
#   src/dict.o —— 会重复定义 dictSdsHash 等符号。这里给它单独的规则：
#   只链 src/sds.o。
tests/test_lookup_order: tests/test_lookup_order.o src/sds.o
	$(CC) $(CFLAGS) -o $@ $^

# ------------------------------------------------------------
# 改造阶段 1：ae 事件循环骨架的空跑验证。
# 独立产物，不动原有 mini-redis —— 便于阶段 0/1 对比。
# ⚠ 必须链 networking.c：server.c 的 free_client 现在转发到 freeClient。
# 用法：make stage1 && ./mini-redis-stage1
STAGE1_SRCS = src/main_stage1.c src/ae.c src/anet.c src/server.c src/networking.c src/protocol.c src/sds.c src/commands.c src/storage.c src/dict.c src/skiplist.c src/zset.c
stage1:
	$(CC) $(CFLAGS) -o mini-redis-stage1 $(STAGE1_SRCS)

# ------------------------------------------------------------
# 跳表算法层微基准：绕开服务端框架（~50k QPS 上限）直接定时 skiplist API，
# 实证 O(1) find / O(log n) range（span）。用法：make benskip 或
#   make benskip RUN=<表大小>     例如 make benskip RUN=400000
# 渐进式 rehash 微基准：量化"一次扩容造成的最大停顿"
# 核心验证：最大停顿是否随 N 线性增长（线性=一次性搬迁；不变=渐进式）
#   make benchrehash RUN=1000000
benchrehash: bench/bench_rehash.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o /tmp/bench_rehash bench/bench_rehash.c src/dict.c src/sds.c
	/tmp/bench_rehash $(RUN)

benskip: bench/bench_skipspan.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o /tmp/bench_skipspan bench/bench_skipspan.c src/skiplist.c src/dict.c src/sds.c
	/tmp/bench_skipspan $(RUN) $(REPS)

# ------------------------------------------------------------
# 自动化集成测试：先编译，再运行 test.sh
test: all
	./test.sh

# 单元测试：逐个运行，任一失败则整体退出码非 0
unit: $(TEST_BINS) test_lookup_order
	@fail=0; \
	for t in $(TEST_BINS) tests/test_lookup_order; do \
		echo "--- 运行 $$t ---"; \
		if ./$$t; then :; else echo ">>> $$t 失败"; fail=1; fi; \
	done; \
	exit $$fail

# 完整验证：单元测试 + 集成回归
check: unit test

# ------------------------------------------------------------
# 内存 / 未定义行为检测：ASan + UBSan
# 用递归 make 传 CFLAGS，避免与目标专属变量、依赖求值顺序纠缠
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-Wall -Wextra -O1 -g -Iinclude -fsanitize=address,undefined -fno-omit-frame-pointer" $(TARGET)
	@echo "已生成带 ASan/UBSan 的 $(TARGET)；可用 SKIP_BUILD=1 ./test.sh 或 make unit 验证"
	@echo "（注意：asan 与普通构建的 .o 不通用，要恢复正常版本请 make clean && make）"

# valgrind 内存检查
# valgrind 不在默认安装里，直接调用只会得到 "No such file or directory"（退出码 127），
# 看不出是"没装"还是"跑失败了"。先探测，再给出可操作的安装提示。
valgrind: $(TARGET)
	@command -v valgrind >/dev/null 2>&1 || { \
		echo "未找到 valgrind。"; \
		echo "  Debian / Ubuntu / WSL：sudo apt install -y valgrind"; \
		echo "  或者改用：make asan（ASan+UBSan，无需额外安装，通常更快更灵敏）"; \
		exit 1; }
	valgrind --leak-check=full --show-leak-kinds=all ./mini-redis

# ------------------------------------------------------------
# 清理规则
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) $(TEST_BINS) tests/*.o tests/*.d

# 彻底清理（含压测产物）
distclean: clean
	rm -f final_bench.csv bench/*.csv

# 伪目标声明（避免目录下恰好有同名文件导致冲突）
.PHONY: all test unit check asan valgrind benskip benchrehash clean distclean test_lookup_order stage1

# 引入自动生成的依赖；文件不存在时静默忽略（首次编译）
-include $(DEPS)
-include $(TEST_DEPS)
