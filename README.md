# Nginx 源码分析与 Web 工程学习

以 Nginx 源码为核心教材，系统学习 Linux 服务器开发、高并发编程和 Web 工程。

## 目录

### Nginx 源码分析笔记

- [Nginx 事件驱动机制](./nginx-event-driven.md) — `ngx_epoll_module.c` 分析，epoll 事件循环原理
- [Nginx Master-Worker 进程模型](./nginx-master-worker.md) — `ngx_process_cycle.c` 分析，多进程架构
- [Nginx 编译部署说明](./部署说明.md) — 从源码编译安装 Nginx

### 实践项目

- [HTTP 正向代理缓存服务器](./http-proxy-cache/) — C++17 实现，从零构建
  - Stage 1：单线程阻塞代理骨架
  - Stage 2：LRU 内存缓存（哈希表 + 链表，O(1)）
  - Stage 3（规划中）：epoll 非阻塞 IO + 线程池 + 黑白名单

## 学习路线

```
Socket 编程基础
  └→ TCP Echo 服务器
      └→ HTTP 正向代理（阻塞版）
          └→ LRU 缓存
              └→ 线程池并发
                  └→ epoll 事件驱动 ← 对照 Nginx 源码
```

## 仓库说明

本仓库同时存放学习笔记和配套实践代码，笔记侧重原理分析，实践代码侧重动手实现。两者互相印证——读 Nginx 源码学设计思想，写代理项目练工程能力。
