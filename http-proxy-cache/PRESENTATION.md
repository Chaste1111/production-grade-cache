# HTTP 代理缓存服务器 — 答辩演示指南

> 从开机到展示完毕，每一步都有

---

## 准备阶段（答辩前5分钟）

```bash
# 1. 打开终端，进入项目目录
cd ~/Lab/http-proxy-cache

# 2. 提前编译好三个阶段（避免现场等）
cd stage1 && make && cd ..
cd stage2 && make && cd ..
cd stage3 && make && cd ..

# 3. 打开三个终端窗口，排好：
#    终端1 — 跑代理
#    终端2 — 发测试请求
#    终端3 — 备用（看日志/面板）

# 4. 打开 Chrome 浏览器
```

---

## 开场（1分钟）

```
"各位老师好，我做的是一个 HTTP 正向代理缓存服务器。

 用 C++17 实现，分三个阶段从零构建：
 单线程阻塞 → 加缓存/过滤/面板 → epoll 非阻塞高并发。

 对标 Nginx 源码，我分析了它的 epoll 事件驱动和 LRU 缓存实现，
 把核心思想落实到代码里。"
```

---

## 第一幕：跑起来（2分钟）

**对着终端操作，边敲边讲：**

```bash
# ─── 启动 Stage3（全功能版本）───
cd stage3
./proxy_server
```

```
屏幕输出：
═══════════════════════════════════
  Stage3 — epoll 非阻塞代理
  代理: 8888  |  面板: 8890
═══════════════════════════════════
```

**讲：**
"代理监听 8888 端口，浏览器配了这个代理就能上网。
同时 8890 是一个实时管理面板。"

---

## 第二幕：基础功能验证（3分钟）

**打开另一个终端：**

```bash
# 1. HTTP 代理 — 访问百度
curl -s -x http://localhost:8888 http://www.baidu.com/ | head -5
```

```
输出：<!DOCTYPE html>...百度一下，你就知道
```

**讲：** "浏览器配 8888 代理，访问百度，代理转发请求并返回结果。"

```bash
# 2. 再访问一次 — 缓存命中
curl -s -x http://localhost:8888 http://www.baidu.com/ | head -3
```

**讲：** "第二次访问同一 URL，代理日志显示 `★ 缓存命中 ★`。百度服务器没有收到任何请求。这就是 LRU 缓存——O(1) 查找，O(1) 淘汰，链表+哈希表实现，跟 Nginx 的思路一样。"

```bash
# 3. 黑名单拦截
curl -s -x http://localhost:8888 http://192.168.1.1/
```

```
输出：Proxy: access denied by filter
```

**讲：** "黑名单在缓存之前检查。即使 192.168.1.1 之前缓存过，也不会返回——防止内网穿透。"

```bash
# 4. HTTPS 隧道
curl -s -x http://localhost:8888 https://www.baidu.com/ | head -3
```

**讲：** "HTTPS CONNECT 隧道——代理发 `200 Connection Established` 后变成透明管道，加密流量双向中继，代理完全不参与加密。"

---

## 第三幕：管理面板（1分钟）

**打开 Chrome，访问 `http://localhost:8890/`**

**讲：**
"实时管理面板。能看到运行状态、缓存使用率、已缓存的 URL 列表、最近 20 条访问日志。HIT 绿色、MISS 紫色、DENY 红色。每 3 秒自动刷新。"

---

## 第四幕：三阶段并发对比（3分钟）

**这是最有力的部分——用数据说话：**

```bash
# 杀掉 Stage3，启动 Stage1
# Ctrl+C 停掉当前代理

cd ../stage1
./proxy_server &
./bench_concurrent.sh
# 记下耗时
kill %1

cd ../stage2
./proxy_server &
./bench_concurrent.sh
# 记下耗时  
kill %1

cd ../stage3
./proxy_server &
./bench_concurrent.sh
# 记下耗时
kill %1
```

**预期结果对比：**

| 阶段 | 架构 | 10并发耗时 |
|------|------|-----------|
| Stage1 | 单线程阻塞 | ~25s |
| Stage2 | select多路复用 | ~30s |
| Stage3 | **epoll非阻塞** | **~3s** |

**讲：**
"同样的10个网站并发访问。Stage1纯串行——一个请求卡住后面全排队。Stage2多了select，但select只管监听端口，处理请求还是阻塞的，而且加了缓存/日志开销。

Stage3用epoll——所有连接同时建立，TCP握手和响应收发交替进行，10个请求3秒内全部返回。这就是Nginx高性能的核心——一个worker进程配一个epoll实例，扛几万并发。"

---

## 第五幕：关键代码展示（2分钟）

**在 VSCode 里快速展示：**

1. **`lru_cache.h`** — "链表+哈希表，所有操作O(1)"
2. **`connection.h`** — "7状态状态机，非阻塞关键"
3. **`epoller.h`** — "epoll三个系统调用的封装"
4. **`proxy_server.cpp:main()`** — "事件循环——epoll_wait醒来处理就绪的连接"

**讲：**
"核心架构就是 Nginx 的思路——epoll 盯所有 fd，非阻塞操作，状态机记录每个连接的进度。只推进一步就回去接着等，绝不阻塞。"

---

## 收尾（30秒）

**讲：**
"总结一下：
1. 从 TCP socket 到 HTTP 协议到 epoll 非阻塞——计算机网络课程的核心知识点都实践了
2. 三个阶段可以独立编译运行，清楚地展示了演化过程
3. 缓存策略（LRU+TTL）、访问控制（黑白名单）、日志记录、管理面板——实验要求的全部功能都已实现
4. 并发测试数据验证了 epoll 相比阻塞版本的巨大优势

请各位老师提问。"

---

## 备用：日志文件现场查看

```bash
cat proxy_access.log
```

```
[2026-05-30 20:15:14] GET http://www.baidu.com/ → HIT (30522 bytes)
[2026-05-30 20:15:15] GET http://192.168.1.1/ → DENIED (103 bytes)
[2026-05-30 20:15:16] CONNECT www.baidu.com:443 → MISS (0 bytes)
```

日期、时间、方法、URL、状态、字节数全部记录。

---

## 备用：15项全功能自动测试

```bash
cd stage3 && ./test_all.sh
```

```
通过: 15  失败: 0
🎉 全部测试通过！
```
