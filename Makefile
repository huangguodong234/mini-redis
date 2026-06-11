# ============================================================
# Makefile for mini-redis (阶段 2: 哈希表版)
# 用法：
#   make        → 编译生成 server
#   make clean  → 删除所有 .o 文件和 server
# ============================================================

# 1. 定义变量：以后换编译器或加参数只需改这里
CC      = gcc                    # 编译器
CFLAGS  = -Wall -g -Iinclude     # 编译选项：-Wall 显示所有警告，-g 加调试信息，-Iinclude 指定头文件目录
TARGET  = server                 # 最终生成的可执行文件名

# 2. 源文件列表（手动指定，因为我们只有 4 个 .c）
SRCS    = src/server.c src/protocol.c src/storage.c src/hashtable.c

# 3. 将 .c 后缀替换为 .o，得到目标文件列表
#    例如：src/server.c → src/server.o
OBJS    = $(SRCS:.c=.o)

# ------------------------------------------------------------
# 默认目标：all 依赖于 $(TARGET)，所以 make 会先生成 server
all: $(TARGET)

# 4. 链接规则：用所有 .o 文件生成最终可执行文件
#    $@ 代表目标（server），$^ 代表所有依赖（$(OBJS) 展开后的 .o 文件）
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# 5. 模式规则：如何从 .c 生成 .o
#    $< 代表第一个依赖（即 .c 文件），$@ 代表目标（.o 文件）
#    -c 表示只编译不链接
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 自动化测试：先编译，再运行 test.sh
test: all
	./test.sh

# 内存检查
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./server

# 清理规则：删除编译产物
clean:
	rm -f $(OBJS) $(TARGET)	

# 彻底清理
distclean: clean
	rm -f test_client test_hash test_hashtable test_resize bench_client

# 伪目标声明（避免目录下恰好有名为 all/clean 的文件导致冲突）
.PHONY: all test valgrind clean distclean
