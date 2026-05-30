# HTTP 代理缓存服务器 — 功能演示指南

> 每个功能都有具体的命令和预期结果

---

## 启动

```bash
cd http-proxy-cache/stage3
make && ./proxy_server
```

```
═══════════════════════════════════
  Stage3 — epoll 非阻塞代理
  代理: 8888  |  面板: 8890
═══════════════════════════════════
```

---

## 1. 基础代理转发

> 浏览器通过代理访问网站，代理转发请求并回传响应

```bash
curl -s -x http://localhost:8888 http://www.baidu.com/ | head -5
```

```
预期：看到百度首页 HTML
<!DOCTYPE html>
<title>百度一下，你就知道</title>
```

**说明：**
```
浏览器(curl) ──→ 代理(8888) ──→ 百度(80)
               解析URL         返回HTML
               转发请求         ←─────
               回传响应
```

---

## 2. 缓存命中 vs 未命中

> 第一次从目标取，第二次直接从内存返回

```bash
# 第1次 — 缓存未命中（连百度）
curl -s -x http://localhost:8888 http://www.baidu.com/ > /dev/null

# 第2次 — 缓存命中（不连百度）
curl -s -x http://localhost:8888 http://www.baidu.com/ > /dev/null
```

代理日志：
```
第1次：[conn 7] 缓存未命中 http://www.baidu.com/     ← 连了百度
       [socket] 正在连接 www.baidu.com:80
       [conn 7] 目标响应收完 (30522 bytes)

第2次：[conn 7] ★ 缓存命中 ★ http://www.baidu.com/   ← 没连百度
       [conn 7] 关闭
```

**说明：第二次请求百度服务器完全不知道——代理直接从本地内存返回。**

---

## 3. 缓存过期（TTL）

```bash
# 第一次访问 — 缓存
curl -s -x http://localhost:8888 http://www.baidu.com/ > /dev/null

# 等 5 分钟（TTL 到期）
echo "等待 300 秒..."
# sleep 300

# 再访问 — 缓存已过期，重新连目标
curl -s -x http://localhost:8888 http://www.baidu.com/ > /dev/null
```

缓存过期后自动淘汰，表现跟第一次未命中一样。

---

## 4. 黑白名单过滤

> 屏蔽特定主机/网段

```bash
# 被屏蔽的内网地址 — 返回 403
curl -s -x http://localhost:8888 http://192.168.1.1/

# 被屏蔽的内网地址
curl -s -x http://localhost:8888 http://10.0.0.1/

# 正常网站 — 不受影响
curl -s -x http://localhost:8888 http://www.baidu.com/ | head -3
```

```
预期：
192.168.1.1 → Proxy: access denied by filter    ← 403 拒绝
10.0.0.1    → Proxy: access denied by filter    ← 403 拒绝
www.baidu.com → <!DOCTYPE html>...              ← 正常通过
```

**说明：黑名单在缓存之前检查——被禁网站的缓存也不会被返回。**

---

## 5. 请求头修改

> 代理在转发时自动添加/删除/修改 HTTP 头

```bash
# 启动一个简单的 echo 服务器来查看代理发出的请求头
# 用 test_target 观察代理转发的请求
```

提前启动 test_target（在 stage1/stage2 目录），然后：

```bash
curl -s -x http://localhost:8888 http://localhost:9999/ > /dev/null
```

目标服务器收到的请求头中包含代理自动添加的：

```
GET / HTTP/1.1
Host: localhost
User-Agent: curl/8.18.0
Accept: */*
X-Proxy-Server: http-proxy-cache/3.0          ← 代理自动添加
X-Forwarded-For: client                        ← 代理自动添加
```

---

## 6. HTTPS CONNECT 隧道

> 代理建立 TCP 隧道，加密流量双向中继

```bash
curl -s -x http://localhost:8888 https://www.baidu.com/ | head -3
```

代理日志：
```
[conn 7] CONNECT 隧道 → www.baidu.com:443
[socket] 正在连接 www.baidu.com:443
[conn 7] TCP握手完成 (target=8)
[conn 7] 隧道已建立，开始双向中继          ← 代理成为透明管道
[conn 7] 关闭
```

**说明：代理看不到加密内容，只是搬运 TCP 字节流。**

---

## 7. 文件日志

```bash
cat proxy_access.log
```

```
[2026-05-30 20:15:14] GET http://www.baidu.com/ → MISS (30522 bytes)
[2026-05-30 20:15:14] GET http://www.baidu.com/ → HIT (30522 bytes)
[2026-05-30 20:15:15] GET http://192.168.1.1/ → DENIED (103 bytes)
[2026-05-30 20:15:16] CONNECT www.baidu.com:443 → MISS (0 bytes)
```

每条记录包含：
- **时间戳**：精确到秒
- **请求方法**：GET / CONNECT
- **目标 URL**：完整地址
- **状态**：MISS(未命中) / HIT(命中) / DENIED(拒绝)
- **字节数**：返回的数据量

---

## 8. 管理面板（浏览器打开）

启动代理后，用浏览器（不配代理）打开：

```
http://localhost:8890/
```

```
📊 HTTP代理缓存服务器
  ├── 运行状态：启动时间、端口号
  ├── 请求统计：命中数 / 未命中数 / 命中率 + 进度条
  ├── 缓存状态：已缓存条数、URL列表
  └── 最近日志：最后20条实时展示（3秒自动刷新）
```

发起几个请求后刷新页面，数字实时变化。

---

## 9. 并发处理

> 同时发多个请求，epoll 交替推进

```bash
# 终端1：启动代理
./proxy_server

# 终端2：同时发 3 个请求
curl -s -x http://localhost:8888 http://www.baidu.com/ > /dev/null &
curl -s -x http://localhost:8888 http://www.baidu.com/ > /dev/null &
curl -s -x http://localhost:8888 https://www.baidu.com/ > /dev/null &
wait
```

代理日志显示 3 个连接同时被处理：

```
[epoll] ADD fd=7 IN    ← conn1
[epoll] ADD fd=8 IN    ← conn2
[epoll] ADD fd=9 IN    ← conn3
[conn 7] 缓存未命中 ...
[conn 8] ★ 缓存命中 ★ ...
[conn 9] CONNECT 隧道 ...
```

**说明：epoll 同时盯 3 个 fd，谁有数据就推进谁，互不阻塞。**

---

## 10. IO 多路复用证据

```bash
# 先发一个慢请求（连不存在的主机，非阻塞 connect 在后台等待）
curl -s --max-time 30 -x http://localhost:8888 http://192.0.2.1:80/ > /dev/null &

# 立即发一个快请求 — 不会被前面的卡住
curl -s -x http://localhost:8888 http://www.baidu.com/ > /dev/null
echo "如果这句立即返回而不是卡30秒，就证明了非阻塞IO在工作"
```

**预期：第二个请求立即返回，不会被第一个慢请求阻塞——这就是 epoll 非阻塞的核心价值。**

---

## 完整演示流程（5分钟走完）

```
1. 启动 ./proxy_server
2. 浏览器打开 http://localhost:8890/  （看到空面板）
3. curl -x 8888 www.baidu.com          （第1次，面板显示 1 MISS）
4. curl -x 8888 www.baidu.com          （第2次，面板显示 1 HIT）
5. curl -x 8888 https://www.baidu.com  （HTTPS 隧道）
6. curl -x 8888 192.168.1.1            （黑名单，面板显示 DENIED）
7. cat proxy_access.log                （看日志）
8. 刷新面板 http://localhost:8890/     （数字全变了）
```
