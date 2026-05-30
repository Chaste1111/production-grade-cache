# Nginx 源码分析与 Web 工程学习

以 Nginx 源码为核心教材，系统学习 Linux 服务器开发、高并发编程和 Web 工程。

## 目录

### Nginx 源码分析笔记

- [Nginx 事件驱动机制](./nginx-event-driven.md) — `ngx_epoll_module.c` 分析，epoll 事件循环原理
- [Nginx Master-Worker 进程模型](./nginx-master-worker.md) — `ngx_process_cycle.c` 分析，多进程架构
- [Nginx 编译部署说明](./部署说明.md) — 从源码编译安装 Nginx

### 实践项目

- [HTTP 正向代理缓存服务器](./http-proxy-cache/) — C++17 实现，4星难度实验作业
  - **Stage 1**：单线程阻塞代理骨架（7模块，正向代理转发）
  - **Stage 2**：LRU 内存缓存 + 黑白名单 + 文件日志 + 双端口管理面板
  - **Stage 3**：epoll 非阻塞 IO + 7状态机 + TTL 过期 + HTTPS CONNECT 隧道

### 架构设计

- [Stage 3 状态机设计图](./STAGE3_STATE_MACHINE.md) — Connection 状态机全图 + epoll 原理

## 实验要求完成度

| 类别 | 功能 | 实现 |
|------|------|------|
| 基础 | HTTP 代理转发 | ✅ 正向代理，浏览器→代理→目标→回传 |
| 基础 | 请求解析与响应回传 | ✅ 7 状态机完整处理 |
| 基础 | 网页资源缓存 | ✅ LRU + TTL(5分钟) |
| 基础 | 日志记录 | ✅ 文件日志（时间/URL/状态/字节数） |
| 基础 | 并发连接 | ✅ epoll 非阻塞 IO，多连接交替推进 |
| 拓展 | 缓存策略 | ✅ LRU 淘汰 + TTL 过期 |
| 拓展 | 黑白名单 | ✅ 网段/主机过滤 |
| 拓展 | 管理界面/统计 | ✅ 8890 面板（命中率+缓存列表+日志） |
| 拓展 | HTTPS CONNECT | ✅ TCP 隧道双向中继 |
| 拓展 | 请求头修改 | ✅ add/remove/replace 规则 |

## 学习路线

```
Socket 编程基础
  └→ TCP Echo 服务器
      └→ HTTP 正向代理（阻塞版）              —— Stage 1
          └→ LRU 缓存 + 黑白名单 + 日志       —— Stage 2
              └→ epoll 事件驱动 + 状态机       —— Stage 3
                  └→ 对照 Nginx 源码（ngx_epoll_module.c）
```

## 仓库说明

本仓库同时存放学习笔记和配套实践代码。代理项目从零构建，三阶段逐步演进：
单线程阻塞 → 模块化缓存 → epoll 非阻塞状态机。完整覆盖实验全部要求。
