# Nginx Master-Worker 进程模型设计思想

> 基于 nginx-1.31.1 源码分析

---

## 一、起点：一个进程为什么不够？

最简单的服务器模型是一个进程处理所有请求，但它有三个致命缺陷：

| 问题 | 后果 |
|------|------|
| 不能利用多核 | 单进程只能跑在一个 CPU 核上，8 核机器浪费 7 个 |
| 稳定性差 | 进程崩了一次，整个服务就挂了 |
| 阻塞扩散 | 哪怕 Nginx 本身是事件驱动的，如果某段代码意外阻塞（磁盘 I/O），所有连接都被波及 |

结论：**需要多个执行实体来分摊负载和风险。**

关键抉择：用多线程还是多进程？

---

## 二、为什么选多进程而非多线程？

这是 Nginx 最根本的设计决策，源自三条判断。

### 2.1 共享状态是万恶之源

多线程共享地址空间，一个线程踩坏了数据结构，所有线程一起崩溃。锁要精心管理，稍有不慎就是死锁或竞争条件。

Nginx 的核心哲学：**让每个执行实体拥有自己完整独立的状态，互不干扰。**

```
多线程:
  Worker线程1 ──┐
  Worker线程2 ──┼── 共享堆/全局变量 ── 一把锁没加对，全局崩溃
  Worker线程3 ──┘

多进程:
  Worker进程1 ─── 独立地址空间 ─── 挂了，重启就是
  Worker进程2 ─── 独立地址空间 ─── 继续运行
  Worker进程3 ─── 独立地址空间 ─── 继续运行
```

### 2.2 Linux 线程本质上就是进程

Linux 上 `pthread_create()` 底层是 `clone(CLONE_VM)`，创建出来的是共享地址空间的轻量进程。调度开销和进程没有本质区别——反正都要进内核调度队列。既然代价差不多，不如选更安全的进程隔离。

### 2.3 天然适配 SO_REUSEPORT

Linux 3.9+ 的 `SO_REUSEPORT` 允许多个进程绑定同一个端口，内核直接做负载均衡。多进程模型完美匹配这个机制，用户态不需要做任何转发：

```
                    ┌──────────┐
  新连接到达:80      │  内核    │
  ──────────────────→│SO_REUSEPORT│
                    │  哈希分发 │
                    └──┬───┬───┘
                       │   │   │
                    Worker Worker Worker
                      0     1     2
```

---

## 三、Master-Worker 的核心思想：关注点分离

决定用多进程后，下一个问题：所有进程都做一样的事情吗？

Nginx 的回答：**不。专门分出一个进程做管理者，其余只管干活。**

```
Master:  "我只管人，不管活。"
Worker:  "我只管活，不管人。"
```

### 3.1 Master — 进程管理者

- 读取和验证配置文件
- 创建、监控、回收 worker 进程
- 接收外部管理信号（reload / reopen / upgrade）
- **不处理任何客户端请求**
- **不监听任何端口**

Master 的主循环极其简单（`src/os/unix/ngx_process_cycle.c:139`）：

```c
for ( ;; ) {
    sigsuspend(&set);     // 阻塞，等待信号唤醒，几乎不消耗 CPU
    // 处理信号事件 ...
}
```

### 3.2 Worker — 请求处理者

- 监听端口，接受客户端连接
- 处理 HTTP / TCP / 邮件请求
- 执行事件循环（epoll_wait → 处理 → epoll_wait → ...）
- **不关心进程管理**
- **不关心配置是否变更**

Worker 的主循环同样极其简单（`src/os/unix/ngx_process_cycle.c:710`）：

```c
for ( ;; ) {
    ngx_process_events_and_timers(cycle);  // 事件循环处理请求
    // 检查信号标志 ...
}
```

### 3.3 分工带来的收益

```
┌──────────────────────────────────────────────────────────────┐
│                       Master 进程 (1个)                       │
│  sigsuspend  ⟶  等信号  ⟶  处理  ⟶  sigsuspend  ⟶  ...       │
│                                                               │
│  职责: fork worker / 回收worker / reload / reopen / upgrade   │
│  CPU:  几乎为零                                               │
└──────────────────────────────────────────────────────────────┘
         │                  │                  │
         │ socketpair       │ socketpair       │ socketpair
         │ (channel)        │ (channel)        │ (channel)
         ▼                  ▼                  ▼
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│  Worker 0   │    │  Worker 1   │    │  Worker 2   │
│ epoll_wait  │    │ epoll_wait  │    │ epoll_wait  │
│    ⟶ 请求   │    │    ⟶ 请求   │    │    ⟶ 请求   │
│    ⟶ 响应   │    │    ⟶ 响应   │    │    ⟶ 响应   │
│ epoll_wait  │    │ epoll_wait  │    │ epoll_wait  │
└─────────────┘    └─────────────┘    └─────────────┘
      独立              独立              独立
     地址空间          地址空间          地址空间
```

每个角色的逻辑都极其简单——简单意味着可靠。

---

## 四、进程间通信：够用就行，绝不多做

### 4.1 控制流：Master → Worker

两条通道，根据场景选用：

| 方式 | 使用场景 | 特点 |
|------|---------|------|
| `kill(pid, signo)` | 快速通知（quit/terminate/reopen） | 内核保证送达 |
| socketpair channel | 传递结构化消息 + 文件描述符 | 可靠，可传 fd |

源码中信号处理函数只做一件事——**设置全局标志位**：

```c
// 信号处理器（极简，只设标志）
sig_atomic_t  ngx_quit;        // SIGQUIT  → 优雅退出
sig_atomic_t  ngx_terminate;   // SIGTERM  → 快速退出
sig_atomic_t  ngx_reconfigure; // SIGHUP   → 重载配置
sig_atomic_t  ngx_reopen;      // SIGUSR1  → 重新打开日志
sig_atomic_t  ngx_change_binary; // SIGUSR2 → 平滑升级
```

Worker 在主循环中检查这些标志并响应——信号处理不做事，主循环做决策。这避免了信号处理器中的重入问题。

### 4.2 数据流：Worker ↔ Worker

Worker 之间几乎不需要通信。少量共享数据放在共享内存中：

```c
// src/event/ngx_event.c — 共享内存布局
shared = shm.addr;                    // ┌──────────────────────┐
ngx_accept_mutex_ptr = shared + 0;    // │ accept 互斥锁         │
ngx_connection_counter = shared + 1;  // │ 连接计数器            │
ngx_temp_number = shared + 2;         // │ 临时随机数种子         │
ngx_stat_accepted = shared + 3;       // │ 统计: 已接受连接       │
ngx_stat_handled = shared + 4;        // │ 统计: 已处理连接       │
// ...                                // │ ...                  │
```

设计原则：**能不同步就不同步，必须同步就用最简单的原子操作或自旋锁。**

---

## 五、热重载（Reload）：新旧交替，不是原地修改

这是 Master-Worker 模式最精彩的设计。实现 reload 有三种思路：

```
方案A: 通知 worker "配置变了，你重新读一下"
  └─ 问题: 如果新配置有错误，正在运行的 worker 状态不一致，回滚困难

方案B: 停掉所有 worker，重新启动
  └─ 问题: 服务中断，正在处理的请求被丢弃

方案C (Nginx): 启动一批新 worker 用新配置，旧 worker 处理完再走
  └─ 零中断，可回滚，配置错误不影响正在运行的服务
```

### 5.1 完整流程

```
nginx -s reload ⟶ Master 收到 SIGHUP:

  步骤1: Master 在隔离的内存中重新解析配置文件
         └─ 配置语法错误? → 日志报错，停止 reload，旧 worker 继续运行

  步骤2: 配置验证通过 → fork 新 worker，新 worker 用新配置启动
         ngx_start_worker_processes(cycle, n, NGX_PROCESS_JUST_RESPAWN)

  步骤3: 等待 100ms 让新 worker 完成初始化
         ngx_msleep(100)

  步骤4: 给旧 worker 发 SIGQUIT
         ngx_signal_worker_processes(cycle, NGX_SHUTDOWN_SIGNAL)

  步骤5: 旧 worker 收到 SIGQUIT 后:
         ├── ngx_close_listening_sockets()  → 不再接受新连接
         ├── ngx_close_idle_connections()   → 关闭 keepalive 空闲连接
         ├── 继续处理正在进行的请求
         └── 所有活跃请求处理完 → 退出

  步骤6: 新 worker 完全接管所有流量
```

### 5.2 为什么叫"无中断"？

```
                    时间线 ───────────────────────────────→

  旧 Worker:   [处理旧请求] ──── [不接新] ──── [处理完剩余] ──── 退出
  新 Worker:            [初始化] ──── [接受新连接，新配置生效]
  
  客户端:     请求 → 旧Worker响应    请求 → 新Worker响应
              完全无感知，没有连接被拒绝，没有请求被丢弃
```

### 5.3 核心思想

**不要原地修改运行时的状态。创建新的状态副本，平滑切换过去。**

这和 Kubernetes 的滚动更新、数据库的蓝绿部署、Erlang/OTP 的热代码升级是同一种思想。

---

## 六、故障容忍：接受崩溃，确保恢复

Nginx 对待崩溃的态度很务实：

> 不试图在代码里防住所有可能的崩溃。接受"总会有意外"，确保挂了能自动恢复。

### 6.1 自动重启机制

```c
// ngx_reap_children (src/os/unix/ngx_process_cycle.c:533)

// 如果一个子进程退出了：
if (ngx_processes[i].respawn           // 它设置了自动重启标记
    && !ngx_processes[i].exiting       // 不是 master 让它退出的
    && !ngx_terminate                  // 不是要关闭整个服务
    && !ngx_quit)
{
    // ★ 重新 fork 一个一模一样的
    ngx_spawn_process(cycle, ngx_processes[i].proc,
                      ngx_processes[i].data,
                      ngx_processes[i].name, i);
    live = 1;
}
```

### 6.2 为什么挂了能直接拉起来？

因为每个 worker 是**无状态的**。Worker 的状态分两类：

| 状态类型 | 存储位置 | crash 后 |
|----------|---------|----------|
| 连接状态 | 每个连接的内存池 | 随进程消失（客户端会重连） |
| 共享状态 | 共享内存 (mmap) | **不受影响** — mmap 映射在进程外 |
| 缓存状态 | 磁盘文件 | **不受影响** — 持久化存储 |

Worker crash → 共享内存和磁盘缓存完好 → 新 worker 无缝接手。最坏情况就是 crash 瞬间那批连接断了（客户端重连即可），而服务本身的正确性不受影响。

### 6.3 为什么多进程比多线程更适合这个模型？

```
多线程 crash:
  一个线程 SIGSEGV
  └─→ 整个进程收到信号
  └─→ 所有线程一起死
  └─→ 所有连接全部断开
  └─→ 无法部分恢复

多进程 crash:
  一个 worker SIGSEGV
  └─→ 只有这个 worker 死
  └─→ 其他 worker 不受影响（继续处理请求）
  └─→ Master 自动 fork 新 worker 补位
  └─→ 服务整体几乎不受影响
```

**进程边界就是故障隔离边界**——这是 Nginx 最核心的可靠性设计。

---

## 七、安全隔离：启动时是 root，干活时是 nobody

Master 以 root 启动，做完以下事情后就不再需要 root 权限：

```
1. 绑定 80 / 443 端口（非 root 无法绑定 < 1024）
2. 打开并写入 PID 文件和日志文件
3. 读取配置文件

然后 fork worker：
  ├── setgid(nginx_group)  → 切换到普通用户组
  └── setuid(nginx_user)   → 切换到普通用户
```

**即使 worker 被攻破，攻击者也只拿到 nginx 用户权限。**不能改配置、不能绑低端口、不能读写系统文件。

```c
// ngx_worker_process_init (src/os/unix/ngx_process_cycle.c:799)
if (geteuid() == 0) {
    // 切换到配置指定的用户
    setgid(ccf->group);
    setuid(ccf->user);
    // 从此刻起，worker 不再是 root
}
```

---

## 八、信号驱动的管理命令

Master 用 `sigsuspend()` 阻塞等待信号，信号到达后设置全局标志位，主循环检查标志并执行对应操作：

| 信号 | 触发方式 | 处理逻辑 |
|------|---------|---------|
| `SIGHUP` | `nginx -s reload` | 重读配置 → fork 新 worker → 旧 worker 优雅退出 |
| `SIGQUIT` | `nginx -s quit` | master 通知 worker 优雅退出，等所有请求处理完 |
| `SIGTERM` | `nginx -s stop` | 快速退出，超时后 SIGKILL 强杀 |
| `SIGUSR1` | `nginx -s reopen` | 关闭旧的日志 fd，打开新的日志文件 |
| `SIGUSR2` | 平滑升级 | 启动新版本 nginx，通过环境变量传递 listen fd |
| `SIGCHLD` | 内部（子进程退出） | 回收子进程，必要时自动重启 |

---

## 九、平滑升级（Binary Upgrade）

升级 nginx 二进制文件时，同样不掉请求：

```
1. 发送 SIGUSR2 给旧 Master
2. 旧 Master fork+exec 新版本 nginx → 新 Master
3. 新 Master 通过环境变量 NGINX_VAR 继承所有 listen fd（不重新 bind）
4. 新 Master fork 新 Worker → 新 Worker 开始处理请求
5. 旧 Master 发送 SIGQUIT 给旧 Worker → 旧 Worker 优雅退出
6. 旧 Master 保留（方便回滚）

需要回滚时:
  → kill 新 Master → 新进程全部退出
  → 发送 SIGWINCH 给旧 Master → 旧 Master 重新拉起 Worker
```

思想：**永远保留旧版本，新版本出问题可以立刻回退。**

---

## 十、总结：六条设计原则

| 原则 | 体现 | 收益 |
|------|------|------|
| **关注点分离** | Master 管进程，Worker 管请求 | 各自逻辑简单，出错面小 |
| **进程隔离** | 每个 worker 独立地址空间 | 一个崩不影响其他，故障域严格隔离 |
| **信号驱动管理** | Master 用 sigsuspend 休眠 | Master 几乎不消耗 CPU |
| **新旧交替** | Reload = 新 worker 替旧 worker | 零中断更新配置 |
| **崩溃自愈** | 接受崩溃，自动拉起 | 不需要防所有 bug，容错成本极低 |
| **最小权限** | Master root 绑端口后 worker 降权 | 安全攻击面最小化 |

### 与传统模型的对比

```
传统 Apache prefork:
  每个请求 → fork 新进程 → 阻塞 I/O → 响应 → 销毁进程
  问题: 频繁 fork + 阻塞 = 高并发下不堪重负

传统多线程:
  一个进程 → N 个线程 → 共享地址空间 → 锁竞争
  问题: 一个线程崩了全挂，锁没加对全挂

Nginx Master-Worker:
  Master(1个) → 只管进程管理，sigsuspend 休眠
  Worker(N个) → 每个独立 epoll 事件循环，非阻塞 I/O
  优势: 隔离 + 事件驱动 + 热更新 + 自愈 + 安全
```

### 关键源码索引

| 功能 | 文件 | 行号 |
|------|------|------|
| `main()` 入口，模式分支 | `src/core/nginx.c` | 196-388 |
| Master 主循环 | `src/os/unix/ngx_process_cycle.c` | 73-275 |
| 创建 Worker | `src/os/unix/ngx_process_cycle.c` | 335-349 |
| Worker 主循环 | `src/os/unix/ngx_process_cycle.c` | 698-748 |
| Worker 初始化（降权等） | `src/os/unix/ngx_process_cycle.c` | 752-936 |
| 回收/重启子进程 | `src/os/unix/ngx_process_cycle.c` | 533-652 |
| 向 Worker 发送信号 | `src/os/unix/ngx_process_cycle.c` | 431-530 |
| Channel 消息处理 | `src/os/unix/ngx_process_cycle.c` | 1000-1085 |
| Channel 传递（通知新进程） | `src/os/unix/ngx_process_cycle.c` | 395-428 |
| 平滑升级（exec 新二进制） | `src/core/nginx.c` | 697-798 |
| 单进程模式对比 | `src/os/unix/ngx_process_cycle.c` | 278-332 |
