# HTTP 正向代理缓存服务器

用 C++17 从零构建的 HTTP 正向代理缓存服务器，作为计算机网络课程实验项目（4星难度），同时也是学习 Linux 服务器开发和高并发编程的实践项目。

## 项目结构

```
http-proxy-cache/
├── stage1/                    # 第一阶段：单线程阻塞代理骨架
│   ├── common.h               #   公共数据结构
│   ├── url_parser.h/cpp       #   模块：URL解析（拆出主机/端口/路径）
│   ├── http_parser.h/cpp      #   模块：HTTP请求行解析
│   ├── socket_util.h/cpp      #   模块：Socket工具（创建/连接）
│   ├── proxy_handler.h/cpp    #   模块：代理核心逻辑
│   ├── proxy_server.cpp       #   主入口
│   ├── test_target.cpp        #   测试用目标服务器
│   └── Makefile
├── stage2/                    # 第二阶段：LRU缓存
│   ├── lru_cache.h/cpp        #   新增：LRU缓存模块（链表+哈希表）
│   ├── proxy_handler.cpp      #   修改：集成缓存查询
│   ├── proxy_server.cpp       #   修改：增加命中率统计
│   └── ...（其余模块同stage1）
└── .gitignore
```

## 功能列表

| 功能 | 状态 | 说明 |
|------|------|------|
| HTTP 正向代理转发 | ✅ | 浏览器 → 代理 → 目标服务器 → 回传 |
| URL 解析 | ✅ | 从完整 URL 拆出主机、端口、路径 |
| HTTP 请求头处理 | ✅ | 请求行解析、头部过滤与重写 |
| 内存缓存 + LRU | ✅ | O(1) 查找、O(1) 淘汰 |
| 缓存命中率统计 | ✅ | 实时显示命中/未命中 |
| 日志记录 | 🚧 | 下阶段 |
| 并发连接 | 🚧 | 线程池 |
| 黑白名单 | 🚧 | 下阶段 |
| HTTPS CONNECT | 🚧 | 下阶段 |

## 快速开始

```bash
# 第一阶段：基础代理
cd stage1
make
# 终端1
./test_target
# 终端2
./proxy_server
# 终端3
curl -x http://localhost:8888 http://localhost:9999/

# 第二阶段：带缓存的代理
cd stage2
make
# 同上步骤启动，多次请求观察缓存命中
```

## 学习路线

三个阶段对应从零学习高并发服务器的完整路径：

```
Socket 基础 → 阻塞 TCP 代理 → LRU 缓存 → 线程池 → epoll 非阻塞
    ↑              ↑               ↑          ↑           ↑
  stage1        stage1          stage2     stage3      stage3
```

每一阶段都是可独立编译运行的完整程序，便于理解演进过程。

## 技术要点

- **LRU 缓存**：`unordered_map` + `list` 组合，哈希表负责 O(1) 查找，链表负责 O(1) 移动和淘汰
- **模块化设计**：URL 解析、HTTP 解析、Socket 工具、代理逻辑各司其职
- **正向代理协议差异**：浏览器发送完整 URL（`GET http://host/path`），代理转发时改为相对路径（`GET /path`）
