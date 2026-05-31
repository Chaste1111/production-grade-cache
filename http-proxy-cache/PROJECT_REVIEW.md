# HTTP 代理缓存服务器 — 项目回顾

> 2026年5月30日，一天完成

---

## 项目概况

```
HTTP 正向代理缓存服务器
  语言：    C++17
  行数：    ~2000 行
  模块：    18 个
  阶段：    3 次架构演进
  测试：    15 项全功能验证通过
  实验要求：10/10 全部完成
```

---

## 三阶段架构演进

```
Stage1 ────────────→ Stage2 ────────────→ Stage3
阻塞代理骨架          +缓存+过滤+日志       epoll非阻塞+状态机
                                              
串行处理              串行处理              并发处理
6模块                 14模块                18模块
~5s/10请求            ~11s/10请求           ~3s/10请求
```

每个阶段独立可编译运行，清晰展示演化路径。

---

## 项目目录

```
http-proxy-cache/
│
├── DESIGN_NOTES.md          # 设计讨论记录（当天所有提问和优化决策）
├── PRESENTATION.md          # 答辩演示指南（5幕流程）
├── DEFENSE_QA.md            # 29个答辩常见问题及参考答案
├── PROJECT_REVIEW.md        # 本文件
│
├── stage1/                  # 第一阶段
│   ├── common.h             #   公共数据结构
│   ├── url_parser.*         #   URL解析
│   ├── http_parser.*        #   HTTP请求解析
│   ├── socket_util.*        #   Socket工具
│   ├── proxy_handler.*      #   代理核心
│   ├── proxy_server.cpp     #   主入口
│   ├── test_target.cpp      #   测试目标服务器
│   ├── bench_concurrent.sh  #   并发压测脚本
│   └── Makefile
│
├── stage2/                  # 第二阶段
│   ├── [stage1全部模块]
│   ├── cache_interface.h    #   缓存接口 ★
│   ├── lru_cache.*          #   LRU实现 ★
│   ├── filter.*             #   黑白名单 ★
│   ├── logger_interface.h   #   日志接口 ★
│   ├── file_logger.*        #   文件日志 ★
│   ├── [proxy_handler修改]  #   集成缓存+过滤+日志
│   ├── [proxy_server修改]   #   select()双端口+管理面板
│   ├── bench_concurrent.sh
│   └── Makefile
│
└── stage3/                  # 第三阶段
    ├── [stage2全部模块]
    ├── connection.*         #   7状态机+隧道处理 ★
    ├── epoller.*            #   epoll封装 ★
    ├── header_mod.*         #   请求头修改 ★
    ├── [lru_cache升级]      #   +TTL过期 ★
    ├── [proxy_server重写]   #   epoll事件循环+可视化面板
    ├── test_all.sh          #   15项全功能测试
    ├── bench_concurrent.sh  #   并发压测脚本
    └── Makefile
```

---

## 核心代码盘点

| 模块 | 文件 | 职责 |
|------|------|------|
| URL解析 | `url_parser.cpp` | `http://host:port/path` → {host, port, path} |
| HTTP解析 | `http_parser.cpp` | 请求行 → {method, url, version} |
| Socket | `socket_util.cpp` | socket创建、非阻塞connect |
| LRU缓存 | `lru_cache.cpp` | 链表+哈希表，O(1)操作，TTL过期 |
| 过滤 | `filter.cpp` | 子串匹配黑/白名单 |
| 日志 | `file_logger.cpp` | 时间戳+格式化写入文件 |
| 头修改 | `header_mod.cpp` | add/remove/replace HTTP头规则 |
| epoll | `epoller.cpp` | epoll_create/ctl/wait 封装 |
| **状态机** | **`connection.cpp`** | **7状态+CONNECT隧道+双向中继** |
| **主循环** | **`proxy_server.cpp`** | **epoll事件循环+可视化面板** |

---

## 技术决策回顾

| 决策 | 选择 | 原因 |
|------|------|------|
| 缓存策略 | LRU + TTL | 符合时间局部性，简单有效 |
| 缓存结构 | 链表+哈希表 | 两者互补，全部O(1) |
| 并发模型 | epoll+非阻塞 | Nginx思路，IO密集型最优解 |
| 模块分离 | 接口+依赖注入 | 换实现只改一行 |
| 黑名单位置 | 缓存之前 | 防止被禁URL缓存命中绕过 |
| 隧道实现 | TCP双向中继 | 不参与TLS，不看不改 |

---

## 学习路径（从提问到理解）

```
上午 — 理解HTTP代理本质
  "这不是反向代理吗？"          → 正向/反向代理的区别
  "阿帕奇不就是正向代理吗"       → Web服务器 vs 代理服务器
  "所以这个项目就是简化的Nginx"  → Nginx是反向代理，方向不同

中午 — 深入缓存
  "为什么用LRU策略？"          → LRU vs LFU vs FIFO
  "为什么用链表？"              → 链表+哈希表互补，O(1)
  "nginx的lru源码是怎样的？"    → Nginx红黑树+队列，共享内存
  "磁盘为什么不用红黑树？"      → B+树，一页塞250个key，矮胖

下午 — 模块化与过滤
  "我建议把缓存单独分离出来"    → 接口化，依赖注入
  "为什么要先查名单再查缓存？"  → 名单是门，缓存是屋
  "127是我的电脑里面才有的吗？" → 本机回环 vs 内网地址段

傍晚 — 并发与epoll
  "select是啥意思？"           → select只盯门，不盯屋里
  "epoll是什么？"              → 内核红黑树+就绪链表
  "非阻塞connect怎么知道成功了？" → EINPROGRESS + EPOLLOUT
  "为什么第二个最慢？"          → 功能最多+串行瓶颈

晚上 — 验证与展示
  "三阶段并发对比"             → 3s vs 11s vs 25s
  "给我答辩指南"               → 5幕流程，29个QA
```

---

## 关键数据

```
代码行数：       ~2000 行 C++（18个模块）
测试脚本：       3个（全功能15项 + 三阶段并发 + 演示脚本）
文档：           5篇（设计笔记 + 网络全景 + 状态机图 + 答辩指南 + QA）
功能完成度：     10/10 实验要求
并发性能：       Stage3 比 Stage2 快 5 倍
GitHub提交：     10+ 次提交
```

---

## 你掌握的知识点

```
TCP/IP 层：
  ✓ 三次握手、四次挥手、TIME_WAIT
  ✓ 非阻塞connect (EINPROGRESS)
  ✓ TCP接收/发送缓冲区
  ✓ socket/bind/listen/accept/connect 全流程

HTTP 层：
  ✓ 请求报文结构（请求行+头部+空行+正文）
  ✓ 响应报文结构（状态行+头部+空行+正文）
  ✓ 正向代理 vs 反向代理（请求行URL区别）
  ✓ CONNECT 隧道（HTTPS代理原理）
  ✓ 常见状态码（200/403/502）

并发编程：
  ✓ 阻塞IO vs 非阻塞IO
  ✓ select vs epoll（O(n) vs O(1)）
  ✓ LT vs ET 触发模式
  ✓ 7状态状态机
  ✓ 依赖注入/接口设计

数据结构：
  ✓ 链表+哈希表 LRU（O(1) 全操作）
  ✓ TTL 过期机制
  ✓ B+树 vs 红黑树（磁盘 vs 内存）
  ✓ Nginx 红黑树+队列对比

工程能力：
  ✓ Makefile 多模块编译
  ✓ 摸块化设计（接口+注入）
  ✓ 命令行测试脚本
  ✓ Git 版本管理
```

---

## 一句话

最初你觉得 `curl -x` 是魔法，最后你写了一个完整的 HTTP 代理服务器，三阶段演进，epoll 扛并发，15 项测试全过。
