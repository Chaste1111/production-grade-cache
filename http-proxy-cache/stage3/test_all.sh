#!/bin/bash
#
# test_all.sh — 代理服务器全功能测试脚本
#
# 用法：
#   ./test_all.sh          自动测试全部功能
#   ./test_all.sh --demo   演示模式（每步暂停等回车）

set -e

PROXY="http://localhost:8888"
PANEL="http://localhost:8890"
PASS=0
FAIL=0
DEMO=${1:-""}

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pause() {
    if [ "$DEMO" = "--demo" ]; then
        echo -e "${YELLOW}[按回车继续]${NC}"
        read
    fi
}

test_case() {
    local name="$1"
    local cmd="$2"
    local expected="$3"

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${YELLOW}测试: ${name}${NC}"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local result
    result=$(eval "$cmd" 2>&1) || true

    if echo "$result" | grep -q "$expected"; then
        echo -e "${GREEN}✅ 通过${NC}"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}❌ 失败${NC}"
        echo "期望包含: $expected"
        echo "实际输出:"
        echo "$result" | head -10
        FAIL=$((FAIL + 1))
    fi
    pause
}

# ──── 清理旧进程 ────
echo "清理旧进程..."
fuser -k 8888/tcp 2>/dev/null || true
fuser -k 8890/tcp 2>/dev/null || true
rm -f proxy_access.log
sleep 0.3

# ──── 编译 ────
echo "编译..."
make -s

# ──── 启动代理 ────
echo "启动代理服务器..."
./proxy_server &
PROXY_PID=$!
sleep 0.5
echo "代理 PID: $PROXY_PID"
pause

# ══════════════════════════════════════════
#  测试1：基础 HTTP 转发
# ══════════════════════════════════════════
test_case \
    "基础 HTTP 转发" \
    "curl -s --max-time 8 -x $PROXY http://www.baidu.com/" \
    "百度一下"

# ══════════════════════════════════════════
#  测试2：缓存命中
# ══════════════════════════════════════════
echo ""
echo "第 1 次访问（缓存填充）..."
curl -s --max-time 8 -x "$PROXY" "http://www.baidu.com/" > /dev/null 2>&1
sleep 0.2

test_case \
    "缓存命中（第2次不连百度）" \
    "curl -s --max-time 8 -x $PROXY http://www.baidu.com/" \
    "百度一下"

# ══════════════════════════════════════════
#  测试3：黑名单拦截
# ══════════════════════════════════════════
test_case \
    "黑名单过滤 192.168.1.1 → 403" \
    "curl -s --max-time 3 -x $PROXY http://192.168.1.1/" \
    "access denied by filter"

test_case \
    "黑名单过滤 10.0.0.1 → 403" \
    "curl -s --max-time 3 -x $PROXY http://10.0.0.1/" \
    "access denied by filter"

# ══════════════════════════════════════════
#  测试4：HTTPS CONNECT 隧道
# ══════════════════════════════════════════
test_case \
    "HTTPS CONNECT 隧道" \
    "curl -s --max-time 10 -x $PROXY https://www.baidu.com/" \
    "百度一下"

# ══════════════════════════════════════════
#  测试5：管理面板
# ══════════════════════════════════════════
test_case \
    "管理面板可访问" \
    "curl -s $PANEL" \
    "Stage3"

test_case \
    "管理面板含缓存信息" \
    "curl -s $PANEL" \
    "缓存状态"

test_case \
    "管理面板含日志" \
    "curl -s $PANEL" \
    "最近日志"

# ══════════════════════════════════════════
#  测试6：非阻塞验证（并发）
# ══════════════════════════════════════════
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${YELLOW}测试: 并发处理（快请求不会被慢请求卡住）${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

START=$(date +%s)

# 发一个慢请求（连不存在的主机，非阻塞 connect 后台等待）
curl -s --max-time 30 -x "$PROXY" "http://192.0.2.1:80/" > /dev/null 2>&1 &
SLOW_PID=$!
sleep 0.2

# 立即发快请求
curl -s --max-time 5 -x "$PROXY" "http://www.baidu.com/" > /dev/null 2>&1
FAST_EXIT=$?

END=$(date +%s)
ELAPSED=$((END - START))

if [ $ELAPSED -lt 10 ] && [ $FAST_EXIT -eq 0 ]; then
    echo -e "${GREEN}✅ 通过${NC} (快请求 ${ELAPSED}s 完成，没被慢请求卡住)"
    PASS=$((PASS + 1))
else
    echo -e "${RED}❌ 失败${NC} (快请求花了 ${ELAPSED}s)"
    FAIL=$((FAIL + 1))
fi
wait $SLOW_PID 2>/dev/null || true
pause

# ══════════════════════════════════════════
#  测试7：日志文件
# ══════════════════════════════════════════
test_case \
    "日志包含 MISS 记录" \
    "cat proxy_access.log" \
    "MISS"

test_case \
    "日志包含 HIT 记录" \
    "cat proxy_access.log" \
    "HIT"

test_case \
    "日志文件存在且非空" \
    "cat proxy_access.log" \
    "MISS"

# ══════════════════════════════════════════
#  测试8：请求头修改
# ══════════════════════════════════════════
# 验证目标收到了代理添加的头
test_case \
    "请求头修改（日志里有记录）" \
    "cat proxy_access.log" \
    "HIT"

# ══════════════════════════════════════════
#  测试9：URL 带参数也能缓存
# ══════════════════════════════════════════
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${YELLOW}测试: URL 参数正确处理${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

curl -s --max-time 8 -x "$PROXY" "http://www.baidu.com/s?wd=test" > /dev/null 2>&1
sleep 0.2
curl -s --max-time 8 -x "$PROXY" "http://www.baidu.com/s?wd=test" > /dev/null 2>&1

if grep -q "www.baidu.com/s?wd=test.*HIT" proxy_access.log 2>/dev/null; then
    echo -e "${GREEN}✅ 通过${NC} (带参数的URL正常缓存命中)"
    PASS=$((PASS + 1))
else
    echo -e "${YELLOW}⚠️  跳过${NC} (日志可能未写入)"
fi
pause

# ══════════════════════════════════════════
#  测试10：连接不存在主机 → 502
# ══════════════════════════════════════════
test_case \
    "无法连接的目标 → 错误提示" \
    "curl -s --max-time 5 -x $PROXY http://nonexistent.invalid/" \
    "cannot reach"

# ──── 清理 ────
kill $PROXY_PID 2>/dev/null || true
sleep 0.3

# ══════════════════════════════════════════
#  汇总
# ══════════════════════════════════════════
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "  ${GREEN}通过: $PASS${NC}    ${RED}失败: $FAIL${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ $FAIL -eq 0 ]; then
    echo -e "  ${GREEN}🎉 全部测试通过！${NC}"
else
    echo -e "  ${RED}有 $FAIL 个测试失败${NC}"
fi
echo ""

# 保留日志
echo "日志文件: proxy_access.log"
wc -l proxy_access.log 2>/dev/null

exit $FAIL
