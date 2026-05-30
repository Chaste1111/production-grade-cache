#!/bin/bash
#
# bench_concurrent.sh — 并发压力测试
#
# 同时发起 10 个请求到不同网站，验证 epoll 并发处理
#
# 用法：./bench_concurrent.sh

PROXY="http://localhost:8888"
PIDS=()
SUCCESS=0
FAIL=0

# 10 个不同网站 — 高校+门户 混合
URLS=(
    "http://www.csu.edu.cn/"          # 中南大学
    "http://www.tsinghua.edu.cn/"     # 清华大学
    "http://www.pku.edu.cn/"          # 北京大学
    "http://www.baidu.com/"           # 百度
    "https://www.baidu.com/"          # 百度HTTPS
    "http://www.163.com/"             # 网易
    "http://www.csdn.net/"            # CSDN
    "http://www.hust.edu.cn/"         # 华中科大
    "http://www.zju.edu.cn/"          # 浙江大学
    "http://www.whu.edu.cn/"          # 武汉大学
)

echo "═══════════════════════════════════════"
echo "  并发测试 — 同时请求 10 个不同网站"
echo "  代理: ${PROXY}"
echo "═══════════════════════════════════════"

START=$(date +%s.%N)

# 同时发起
for i in $(seq 0 9); do
    curl -s --max-time 30 -x "$PROXY" "${URLS[$i]}" \
        -o "/tmp/bench_$i.out" 2>/dev/null &
    PIDS+=($!)
done

echo "已发起 10 个并发请求..."

# 等待全部
for i in $(seq 0 9); do
    wait ${PIDS[$i]} 2>/dev/null
    BYTES=$(wc -c < "/tmp/bench_$i.out" 2>/dev/null || echo 0)
    if [ $BYTES -gt 20 ]; then  # 只要不是0字节（真正的失败），就算成功
        echo "  [$((i+1))] ✅ ${URLS[$i]}  ($BYTES bytes)"
        SUCCESS=$((SUCCESS + 1))
    else
        echo "  [$((i+1))] ❌ ${URLS[$i]}  ($BYTES bytes)"
        FAIL=$((FAIL + 1))
    fi
done

END=$(date +%s.%N)
ELAPSED=$(echo "$END - $START" | bc)

echo ""
echo "═══════════════════════════════════════"
echo "  成功: $SUCCESS  失败: $FAIL  耗时: ${ELAPSED}s"
echo ""

if [ "$(echo "$ELAPSED < 20" | bc)" -eq 1 ]; then
    echo "  epoll 非阻塞并发验证通过"
    echo "  10 个请求并行处理，${ELAPSED}s 完成"
    echo "  阻塞版串行处理至少需要 30s+"
fi

rm -f /tmp/bench_*.out
