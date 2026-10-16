#!/usr/bin/env bash
# ============================================================
# setup_ab.sh —— 准备 A/B 对比环境
# ============================================================
#
# A = 阻塞版（改造前，epoll 改造之前的设计：accept 一个就陷进读循环）
# B = epoll 版（当前工作区）
#
# ★ 为什么 A 要从 git 历史里取，而不是在工作区留一份：
#   留一份就意味着两份代码要同步维护（改一处漏一处），
#   而 A 的价值只是「作为对照物被测量一次」。git 历史就是它的正确归宿。
#
# 用法：
#   ./bench/setup_ab.sh          # 准备两个可执行文件
#   ./bench/setup_ab.sh --clean  # 清理
#
# 产物：
#   /tmp/ab/blocking/mini-redis   ← A
#   /tmp/ab/epoll/mini-redis      ← B
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AB_DIR="/tmp/ab"
# ★ A 版所在提交：阶段 1 的前一个提交（此时还没有事件循环）
BASE_COMMIT="${BASE_COMMIT:-c49f44c}"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$AB_DIR"
    echo "已清理 $AB_DIR"
    exit 0
fi

echo "==> 准备 A/B 环境（A 取自 $BASE_COMMIT）"
rm -rf "$AB_DIR"
mkdir -p "$AB_DIR/blocking" "$AB_DIR/epoll"

# ---- A：阻塞版（从 git 历史导出后编译）----
git -C "$REPO_ROOT" archive "$BASE_COMMIT" | tar -x -C "$AB_DIR/blocking" || {
    echo "错误：无法导出 $BASE_COMMIT" >&2; exit 1; }
( cd "$AB_DIR/blocking" && make >/dev/null 2>&1 ) || {
    echo "错误：A（阻塞版）编译失败" >&2; exit 1; }
echo "    A（阻塞版） : $AB_DIR/blocking/mini-redis"

# ---- B：epoll 版（当前工作区）----
( cd "$REPO_ROOT" && make >/dev/null 2>&1 ) || {
    echo "错误：B（epoll 版）编译失败" >&2; exit 1; }
cp "$REPO_ROOT/mini-redis" "$AB_DIR/epoll/mini-redis"
echo "    B（epoll 版）: $AB_DIR/epoll/mini-redis"

echo
echo "==> 环境就绪"
echo "    A 的源文件数 = $(ls "$AB_DIR/blocking/src"/*.c | wc -l)（无 networking.c / ae.c）"
echo "    B 的源文件数 = $(ls "$REPO_ROOT/src"/*.c | wc -l)"
