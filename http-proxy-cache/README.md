# HTTP 正向代理缓存服务器

C++17 实现，计算机网络课程实验项目（4星难度）。三阶段从零构建，逐步演进到 epoll 非阻塞事件驱动架构。

## 项目结构

```
http-proxy-cache/
├── DESIGN_NOTES.md      # 设计讨论记录（5月30日）
├── stage1/              # 第一阶段：单线程阻塞代理骨架
│   ├── common.h         #   公共数据结构
│   ├── url_parser.*     #   URL 解析模块
│   ├── http_parser.*    #   HTTP 请求解析模块
│   ├── socket_util.*    #   Socket 工具模块
│   ├── proxy_handler.*  #   代理核心逻辑
│   ├── proxy_server.cpp #   主入口
│   ├── test_target.cpp  #   测试目标服务器
│   └── Makefile
├── stage2/              # 第二阶段：LRU 缓存 + 过滤 + 日志 + 面板
│   ├── cache_interface.h #  缓存抽象接口 ★
│   ├── lru_cache.*       #  LRU 实现（链表+哈希表，O(1)）★
│   ├── filter.*          #  黑白名单过滤 ★
│   ├── logger_interface.h # 日志抽象接口 ★
│   ├── file_logger.*     #  文件日志实现 ★
│   ├── proxy_handler.*   #  修改：集成缓存+过滤+日志
│   ├── proxy_server.cpp  #  双端口 select() 架构
│   └── ...
└── stage3/              # 第三阶段：epoll 非阻塞 + 状态机 + TTL + HTTPS
    ├── connection.*      #  Connection 状态机（7状态）★
    ├── epoller.*         #  epoll 封装 ★
    ├── header_mod.*      #  请求头修改模块 ★
    ├── lru_cache.*       #  LRU + TTL 过期 ★
    ├── proxy_server.cpp  #  事件循环主程序
    └── ...
```

## 功能列表

| 功能 | Stage1 | Stage2 | Stage3 |
|------|--------|--------|--------|
| HTTP 正向代理转发 | ✅ | ✅ | ✅ |
| URL / HTTP 解析 | ✅ | ✅ | ✅ |
| LRU 内存缓存 | — | ✅ | ✅ |
| TTL 缓存过期 | — | — | ✅ |
| 文件日志 | — | ✅ | ✅ |
| 黑白名单过滤 | — | ✅ | ✅ |
| 管理面板 (8890) | — | ✅ | ✅ |
| 请求头修改 | — | — | ✅ |
| HTTPS CONNECT | — | — | ✅ |
| select 多端口 | — | ✅ | — |
| epoll 非阻塞 IO | — | — | ✅ |
| 连接状态机 | — | — | ✅ |

## 快速开始

```bash
# 各阶段独立编译运行
cd stage1 && make && ./proxy_server    # 阻塞版
cd stage2 && make && ./proxy_server    # 缓存+面板
cd stage3 && make && ./proxy_server    # epoll非阻塞（推荐）

# 测试
curl -x http://localhost:8888 http://www.baidu.com/    # HTTP 代理
curl -x http://localhost:8888 https://www.baidu.com/   # HTTPS 隧道
curl http://localhost:8890/                             # 管理面板
```

## 学习路线图

```
Socket 基础 → 阻塞 TCP 代理 → LRU 缓存 → select 多路复用 → epoll 非阻塞状态机
    ↑              ↑              ↑            ↑                ↑
  Stage1        Stage1         Stage2       Stage2            Stage3
```

三个阶段对应从零学习高并发服务器的完整路径，每阶段独立可编译运行。
