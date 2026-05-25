# Nginx 事件驱动 + 非阻塞 I/O 深度分析

> 基于 nginx-1.31.1 源码，逐函数逐行分析。

---

## 一、核心数据结构

### 1.1 事件结构体 `ngx_event_t`

**定义位置**: `src/event/ngx_event.h:30-138`

```c
struct ngx_event_s {
    void            *data;       // 指向所属的 ngx_connection_t
    unsigned         write:1;    // 0=读事件  1=写事件
    unsigned         accept:1;   // 是否是 listen socket 的 accept 事件
    unsigned         instance:1; // 用于检测"过期事件"(stale event)
    unsigned         active:1;   // 是否已注册到内核事件过滤器(epoll/kqueue)
    unsigned         disabled:1; // 是否被暂时禁用
    unsigned         ready:1;    // 事件是否就绪(数据可读/可写)
    unsigned         oneshot:1;  // 单次触发(Solaris eventport)
    unsigned         complete:1; // aio 操作完成标记
    unsigned         eof:1;      // 读到文件末尾
    unsigned         error:1;    // 出错标记
    unsigned         timedout:1; // 是否已超时
    unsigned         timer_set:1;// 是否已加入定时器红黑树
    unsigned         delayed:1;  // 延迟处理标记
    unsigned         deferred_accept:1; // 延迟 accept
    unsigned         pending_eof:1;     // kqueue/epoll 报告的 EOF
    unsigned         posted:1;   // 是否已投递到 posted 队列
    unsigned         closed:1;   // 事件已关闭

    int              available;  // kqueue:可accept数/可读字节数
                                 // epoll: -1 表示未知
    ngx_event_handler_pt  handler; // ★ 事件回调函数指针

    ngx_uint_t       index;      // 事件索引
    ngx_log_t       *log;        // 日志对象

    ngx_rbtree_node_t   timer;   // ★ 内嵌红黑树节点（用于超时管理）

    ngx_queue_t      queue;      // ★ 内嵌队列节点（用于 posted 事件队列）
};
```

**设计要点**：
- 全部用位域压缩，一个事件结构体只占少量内存
- 同时内嵌红黑树节点和队列节点，一个结构体参与两种数据结构
- `handler` 函数指针是实现"事件驱动"的关键——不同事件绑定不同回调

### 1.2 连接结构体 `ngx_connection_t`

**定义位置**: `src/core/ngx_connection.h:127-206`

```c
struct ngx_connection_s {
    void               *data;      // 上层协议私有数据(http/stream/mail)
    ngx_event_t        *read;      // ★ 读事件
    ngx_event_t        *write;     // ★ 写事件
    ngx_socket_t        fd;        // 文件描述符

    ngx_recv_pt         recv;       // ★ 非阻塞 recv 函数指针
    ngx_send_pt         send;       // ★ 非阻塞 send 函数指针
    ngx_recv_chain_pt   recv_chain; // 非阻塞 recv_chain
    ngx_send_chain_pt   send_chain; // 非阻塞 send_chain

    ngx_listening_t    *listening;  // 指向监听 socket
    ngx_pool_t         *pool;       // ★ 每个连接独立的内存池

    off_t               sent;       // 已发送字节数
    ngx_log_t          *log;
    ngx_atomic_uint_t   number;     // 连接编号
    ngx_msec_t          start_time; // 连接建立时间
    ngx_uint_t          requests;   // 该连接处理的请求数

    unsigned            buffered:8; // 缓冲区状态标记
    unsigned            timedout:1;
    unsigned            error:1;
    unsigned            destroyed:1;
    unsigned            idle:1;     // 空闲连接(keepalive)
    unsigned            reusable:1; // 可复用连接
    unsigned            close:1;

    unsigned            sendfile:1;
    unsigned            tcp_nodelay:2;
    unsigned            tcp_nopush:2;
};
```

**设计要点**：
- 每个连接同时持有读事件指针和写事件指针
- `recv`/`send` 是函数指针，实际指向非阻塞版本的 socket 操作
- 独立的 `pool` 内存池：连接关闭时一次性释放所有内存

### 1.3 事件操作接口 `ngx_event_actions_t`

**定义位置**: `src/event/ngx_event.h:166-183`

```c
typedef struct {
    ngx_int_t  (*add)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*del)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*enable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*disable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*add_conn)(ngx_connection_t *c);
    ngx_int_t  (*del_conn)(ngx_connection_t *c, ngx_uint_t flags);
    ngx_int_t  (*notify)(ngx_event_handler_pt handler);
    ngx_int_t  (*process_events)(ngx_cycle_t *cycle, ngx_msec_t timer,
                                 ngx_uint_t flags);  // ★ 核心：等待并处理事件
    ngx_int_t  (*init)(ngx_cycle_t *cycle, ngx_msec_t timer);
    void       (*done)(ngx_cycle_t *cycle);
} ngx_event_actions_t;
```

通过宏将接口绑定到具体实现：

```c
#define ngx_process_events   ngx_event_actions.process_events
#define ngx_add_event        ngx_event_actions.add
#define ngx_del_event        ngx_event_actions.del
```

**这是多态的设计**：编译时确定用 epoll/kqueue/select，运行时通过函数指针调用。

---

## 二、事件处理标志位

**定义位置**: `src/event/ngx_event.h:196-268`

| 标志位 | 值 | 含义 | 适用机制 |
|--------|-----|------|----------|
| `NGX_USE_LEVEL_EVENT` | 0x00000001 | LT 水平触发：只要缓冲区有数据就反复通知 | select, poll, /dev/poll |
| `NGX_USE_ONESHOT_EVENT` | 0x00000002 | 触发一次后自动从内核删除 | kqueue, epoll |
| `NGX_USE_CLEAR_EVENT` | 0x00000004 | ET 边缘触发：仅状态变化时通知一次 | **kqueue, epoll** |
| `NGX_USE_KQUEUE_EVENT` | 0x00000008 | 具有 kqueue 特性(eof/errno/available) | kqueue |
| `NGX_USE_LOWAT_EVENT` | 0x00000010 | 支持低水位标记 | kqueue NOTE_LOWAT |
| `NGX_USE_GREEDY_EVENT` | 0x00000020 | **贪婪模式**：读写直到 EAGAIN | **epoll ET 必需** |
| `NGX_USE_EPOLL_EVENT` | 0x00000040 | 表示当前使用 epoll | epoll |
| `NGX_USE_IOCP_EVENT` | 0x00000200 | IOCP 模式(句柄只添加一次) | Windows IOCP |
| `NGX_USE_FD_EVENT` | 0x00000400 | 需要文件描述符表(无 opaque data) | poll, /dev/poll |
| `NGX_USE_TIMER_EVENT` | 0x00000800 | 内核自行管理定时器 | kqueue, eventport |
| `NGX_USE_EVENTPORT_EVENT` | 0x00001000 | 通知后所有 fd 的过滤器都删除 | Solaris eventport |
| `NGX_USE_VNODE_EVENT` | 0x00002000 | 支持 vnode 通知 | kqueue |

**epoll 初始化时的标志组合** (`ngx_epoll_init`, `src/event/modules/ngx_epoll_module.c:323-380`):

```c
ngx_event_flags = NGX_USE_CLEAR_EVENT     // ET 边缘触发
                | NGX_USE_GREEDY_EVENT    // 贪婪读写
                | NGX_USE_EPOLL_EVENT;    // 标识这是 epoll
```

---

## 三、事件循环主函数 —— 架构的心脏

**定义位置**: `src/event/ngx_event.c:194-264`

每个 worker 进程的主循环就是这个函数，它驱动了 Nginx 所有的 I/O 操作：

```c
void
ngx_process_events_and_timers(ngx_cycle_t *cycle)
{
    ngx_uint_t  flags;
    ngx_msec_t  timer, delta;

    // ===== 步骤1: 计算 epoll_wait 的超时时间 =====
    if (ngx_timer_resolution) {
        timer = NGX_TIMER_INFINITE;   // 使用 SIGALRM 定时器时传无限
        flags = 0;
    } else {
        timer = ngx_event_find_timer(); // ★ 从红黑树取最近的超时时间
        flags = NGX_UPDATE_TIME;        // 通知 process_events 更新缓存时间
    }

    // ===== 步骤2: accept 互斥锁（多 worker 时避免惊群） =====
    if (ngx_use_accept_mutex) {
        if (ngx_accept_disabled > 0) {
            ngx_accept_disabled--;      // 当前 worker 太忙，放弃本轮 accept
        } else {
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) return;

            if (ngx_accept_mutex_held) {
                flags |= NGX_POST_EVENTS;  // 拿到锁→延迟处理事件
            } else {
                // 没拿到锁→缩短等待时间，尽快重试抢锁
                if (timer == NGX_TIMER_INFINITE
                    || timer > ngx_accept_mutex_delay)
                {
                    timer = ngx_accept_mutex_delay;
                }
            }
        }
    }

    // ===== 步骤3: 处理上一轮剩余的 posted_next 事件 =====
    if (!ngx_queue_empty(&ngx_posted_next_events)) {
        ngx_event_move_posted_next(cycle);
        timer = 0;  // 有延迟事件要处理，不等待
    }

    // ===== 步骤4: ★★★ 核心：调用 epoll_wait/kqueue 等待事件 ★★★ =====
    delta = ngx_current_msec;
    (void) ngx_process_events(cycle, timer, flags);
    //       ↑ 展开为 ngx_event_actions.process_events
    //         在 Linux 上即 ngx_epoll_process_events()
    delta = ngx_current_msec - delta;   // 统计 epoll_wait 耗时

    // ===== 步骤5: 处理 accept 事件（新连接） =====
    ngx_event_process_posted(cycle, &ngx_posted_accept_events);

    // ===== 步骤6: 释放 accept 互斥锁 =====
    if (ngx_accept_mutex_held) {
        ngx_shmtx_unlock(&ngx_accept_mutex);
    }

    // ===== 步骤7: ★ 处理到期的定时器事件 =====
    ngx_event_expire_timers();

    // ===== 步骤8: 处理普通读写事件 =====
    ngx_event_process_posted(cycle, &ngx_posted_events);
}
```

### 执行流程图

```
┌──────────────────────────────────────────────────────────────────┐
│                    Worker 进程主循环                               │
│                                                                   │
│  ┌─────────────────┐                                              │
│  │ 1. 从红黑树找    │   ngx_event_find_timer()                    │
│  │    最近超时时间   │   返回值 → timer (ms)                       │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│  ┌─────────────────┐                                              │
│  │ 2. 抢 accept 锁  │   ngx_trylock_accept_mutex()                │
│  │    (多 worker)    │   拿锁的负责accept，没拿到的处理已有连接    │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│  ┌─────────────────┐                                              │
│  │ 3. epoll_wait() │   ★ 核心阻塞点                               │
│  │    等待事件到来   │   超时时间 = timer 或 NGX_TIMER_INFINITE     │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│  ┌─────────────────┐                                              │
│  │ 4. 遍历就绪事件  │   循环处理 event_list[]                      │
│  │    投递到队列    │   → ngx_posted_accept_events (新连接)        │
│  │                  │   → ngx_posted_events (读写事件)             │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│  ┌─────────────────┐                                              │
│  │ 5. 处理 posted   │   ngx_event_process_posted()                │
│  │    accept 队列   │   每个事件调用 rev->handler(rev)             │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│  ┌─────────────────┐                                              │
│  │ 6. 释放accept锁  │   ngx_shmtx_unlock()                        │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│  ┌─────────────────┐                                              │
│  │ 7. 处理超时事件  │   ngx_event_expire_timers()                 │
│  │    遍历红黑树    │   到期节点的 handler(ev)                      │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│  ┌─────────────────┐                                              │
│  │ 8. 处理 posted   │   ngx_event_process_posted()                │
│  │    读写事件队列  │   每个事件调用 ev->handler(ev)               │
│  └────────┬────────┘                                              │
│           ↓                                                       │
│        回到步骤1，循环                                              │
└──────────────────────────────────────────────────────────────────┘
```

---

## 四、epoll 模块实现（Linux 核心路径）

**源码文件**: `src/event/modules/ngx_epoll_module.c`

### 4.1 静态变量

```c
static int                  ep = -1;        // epoll 文件描述符
static struct epoll_event  *event_list;     // 就绪事件数组
static ngx_uint_t           nevents;        // 数组大小
```

### 4.2 模块注册

```c
static ngx_event_module_t  ngx_epoll_module_ctx = {
    &epoll_name,
    ngx_epoll_create_conf,
    ngx_epoll_init_conf,
    {
        ngx_epoll_add_event,             // add
        ngx_epoll_del_event,             // del
        ngx_epoll_add_event,             // enable
        ngx_epoll_del_event,             // disable
        ngx_epoll_add_connection,        // add_conn
        ngx_epoll_del_connection,        // del_conn
        ngx_epoll_notify,                // notify
        ngx_epoll_process_events,        // process_events  ★
        ngx_epoll_init,                  // init
        ngx_epoll_done,                  // done
    }
};
```

### 4.3 初始化 (`ngx_epoll_init`, 行 323-380)

```c
static ngx_int_t
ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_epoll_conf_t  *epcf;

    epcf = ngx_event_get_conf(cycle->conf_ctx, ngx_epoll_module);

    if (ep == -1) {
        // ★ 创建 epoll 实例
        ep = epoll_create(cycle->connection_n / 2);
        if (ep == -1) {
            return NGX_ERROR;
        }
    }

    // 分配就绪事件数组
    if (nevents < epcf->events) {
        if (event_list) ngx_free(event_list);
        event_list = ngx_alloc(sizeof(struct epoll_event) * epcf->events,
                               cycle->log);
    }

    nevents = epcf->events;

    // ★ 将全局事件操作表指向 epoll 的实现
    ngx_event_actions = ngx_epoll_module_ctx.actions;

    // ★ 设置核心标志
    ngx_event_flags = NGX_USE_CLEAR_EVENT      // ET 边缘触发
                    | NGX_USE_GREEDY_EVENT     // 贪婪读写
                    | NGX_USE_EPOLL_EVENT;     // 标记为 epoll

    return NGX_OK;
}
```

### 4.4 添加事件 (`ngx_epoll_add_event`, 行 579-642)

```c
static ngx_int_t
ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    int                  op;
    uint32_t             events, prev;
    ngx_event_t         *e;
    ngx_connection_t    *c;
    struct epoll_event   ee;

    c = ev->data;  // 从事件取回连接

    // 构造 epoll 事件掩码
    events = (uint32_t) event;  // EPOLLIN 或 EPOLLOUT

    if (event == NGX_READ_EVENT) {
        e = c->write;
        prev = EPOLLOUT;
    } else {
        e = c->read;
        prev = EPOLLIN;
    }

    // 如果另一个方向的事件已经 active，用 EPOLL_CTL_MOD
    // 否则用 EPOLL_CTL_ADD
    if (e->active) {
        op = EPOLL_CTL_MOD;
        events |= prev;
    } else {
        op = EPOLL_CTL_ADD;
    }

    // ★ ET 模式：EPOLLET
    ee.events = events | (uint32_t) flags;  // flags 传入 EPOLLET
    // ★ 关键：ptr 指向 connection，以便事件触发时找回
    //     最低位用来存 instance（检测过期事件）
    ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);

    if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
        return NGX_ERROR;
    }

    ev->active = 1;  // 标记事件已注册到内核

    return NGX_OK;
}
```

### 4.5 核心：等待并分发事件 (`ngx_epoll_process_events`, 行 784-913)

```c
static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
                         ngx_uint_t flags)
{
    int                events;
    uint32_t           revents;
    ngx_int_t          instance, i;
    ngx_event_t       *rev, *wev;
    ngx_queue_t       *queue;
    ngx_connection_t  *c;

    // ★★★ 阻塞等待事件，timer 是超时时间(ms) ★★★
    //      NGX_TIMER_INFINITE = -1 → 无限等待
    //      timer = 0              → 立即返回(非阻塞)
    //      timer > 0              → 等待指定毫秒数
    events = epoll_wait(ep, event_list, (int) nevents, timer);

    err = (events == -1) ? ngx_errno : 0;

    // 如果需要，更新缓存时间
    if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
        ngx_time_update();
    }

    // 错误处理
    if (err) {
        if (err == NGX_EINTR) {
            // 被信号中断，正常情况
            if (ngx_event_timer_alarm) {
                ngx_event_timer_alarm = 0;
                return NGX_OK;
            }
            level = NGX_LOG_INFO;
        } else {
            level = NGX_LOG_ALERT;
        }
        ngx_log_error(level, cycle->log, err, "epoll_wait() failed");
        return NGX_ERROR;
    }

    // timeout 到期（没有事件）
    if (events == 0) {
        if (timer != NGX_TIMER_INFINITE) {
            return NGX_OK;  // 正常的超时返回
        }
        return NGX_ERROR;   // 无限等待却返回了，异常
    }

    // ★ 遍历所有就绪事件
    for (i = 0; i < events; i++) {
        // 从 epoll_event.data.ptr 取回 connection
        c = event_list[i].data.ptr;

        // ★ instance 机制：检测过期事件
        //    连接关闭后 fd 可能被重用，instance 用来判断事件是否属于当前连接
        instance = (uintptr_t) c & 1;
        c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);

        rev = c->read;

        // ★ 过期事件检测
        if (c->fd == -1 || rev->instance != instance) {
            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "epoll: stale event %p", c);
            continue;  // 忽略过期事件
        }

        revents = event_list[i].events;

        // ★ 错误处理：EPOLLERR/EPOLLHUP 时同时标记读写就绪
        if (revents & (EPOLLERR|EPOLLHUP)) {
            revents |= EPOLLIN|EPOLLOUT;
        }

        // ★ 处理读就绪
        if ((revents & EPOLLIN) && rev->active) {
            rev->ready = 1;       // 标记就绪
            rev->available = -1;  // -1 表示未知可用字节数

            if (flags & NGX_POST_EVENTS) {
                // accept 事件和普通事件投递到不同队列
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;
                ngx_post_event(rev, queue);
            } else {
                rev->handler(rev);  // ★ 直接调用回调
            }
        }

        wev = c->write;

        // ★ 处理写就绪
        if ((revents & EPOLLOUT) && wev->active) {
            if (c->fd == -1 || wev->instance != instance) {
                continue;  // 过期写事件
            }

            wev->ready = 1;

            if (flags & NGX_POST_EVENTS) {
                ngx_post_event(wev, &ngx_posted_events);
            } else {
                wev->handler(wev);  // ★ 直接调用回调
            }
        }
    }

    return NGX_OK;
}
```

**关键设计点**：

1. **`ee.data.ptr = (void *)((uintptr_t)c | ev->instance)`** — 把 connection 指针和 instance 位打包到一个指针里。取回时用位运算分离。连接关闭后 fd 可能被复用，instance 机制防止处理属于旧连接的事件。

2. **accept 事件单独队列** (`ngx_posted_accept_events`) — accept 互斥锁持有期间，新连接事件延迟到锁释放后处理，保证锁持有时间最短。

3. **`rev->ready = 1` 然后直接调 handler** — 不是在此函数中做实际 I/O，而是交给上层回调。这种设计让事件层和协议层完全解耦。

---

## 五、非阻塞 I/O 全链路

### 5.1 Accept 阶段：非阻塞接受新连接

**定义位置**: `src/event/ngx_event_accept.c:20-341`

```c
void
ngx_event_accept(ngx_event_t *ev)
{
    socklen_t          socklen;
    ngx_err_t          err;
    ngx_socket_t       s;
    ngx_connection_t  *c, *lc;
    ngx_listening_t   *ls;

    lc = ev->data;          // listen socket 的 connection
    ls = lc->listening;
    ev->ready = 0;          // 清除就绪标记

    do {                    // ★ do-while: multi_accept 时一次处理多个连接
        socklen = sizeof(ngx_sockaddr_t);

#if (NGX_HAVE_ACCEPT4)
        // ★ accept4 + SOCK_NONBLOCK：一步得到非阻塞 socket
        s = accept4(lc->fd, &sa.sockaddr, &socklen, SOCK_NONBLOCK);
#else
        s = accept(lc->fd, &sa.sockaddr, &socklen);
#endif

        if (s == (ngx_socket_t) -1) {
            err = ngx_socket_errno;

            if (err == NGX_EAGAIN) {
                // ★ 没有更多连接了，直接返回
                return;
            }

            // ★★★ 文件描述符耗尽保护 ★★★
            if (err == NGX_EMFILE || err == NGX_ENFILE) {
                // 1. 禁用所有 listen socket 的 accept 事件
                ngx_disable_accept_events((ngx_cycle_t *) ngx_cycle, 1);

                if (ngx_use_accept_mutex) {
                    // 2. 释放 accept 互斥锁(让其他 worker 处理)
                    ngx_shmtx_unlock(&ngx_accept_mutex);
                    ngx_accept_mutex_held = 0;
                    ngx_accept_disabled = 1;
                } else {
                    // 3. 设置定时器，延后重试
                    ngx_add_timer(ev, ecf->accept_mutex_delay);
                }
            }

            return;
        }

        // ★ 计算负载：如果空闲连接不足 1/8，标记 disabled
        ngx_accept_disabled = ngx_cycle->connection_n / 8
                              - ngx_cycle->free_connection_n;

        // 为每个新连接从连接池分配 connection
        c = ngx_get_connection(s, ev->log);
        if (c == NULL) {
            ngx_close_socket(s);
            return;
        }

        // ★★★ 为每个连接创建独立内存池 ★★★
        c->pool = ngx_create_pool(ls->pool_size, ev->log);

        // ★★★ 设置非阻塞 I/O 函数指针 ★★★
        c->recv = ngx_recv;           // 非阻塞 recv
        c->send = ngx_send;           // 非阻塞 send
        c->recv_chain = ngx_recv_chain;
        c->send_chain = ngx_send_chain;

        // ★ 标记写事件就绪(可以开始读请求了)
        wev = c->write;
        wev->ready = 1;

        c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
        c->start_time = ngx_current_msec;

        // ★★★ 交给上层协议处理 ★★★
        //     对于 HTTP: 指向 ngx_http_init_connection
        //     对于 Stream: 指向 ngx_stream_init_connection
        //     对于 Mail: 指向 ngx_mail_init_connection
        ls->handler(c);

    } while (ev->available);  // multi_accept: 一次处理多个连接
}
```

**流程图**：

```
epoll_wait 返回 EPOLLIN (listen fd)
         │
         ▼
  rev->handler = ngx_event_accept()
         │
         ├── accept4(fd, SOCK_NONBLOCK) → 新 socket (s)
         │      │
         │      ├── s == -1, EAGAIN → return (没更多连接)
         │      ├── s == -1, EMFILE → 禁用accept+设定时器
         │      └── s >= 0 → 继续
         │
         ├── ngx_get_connection(s) → 从连接池取 connection
         ├── ngx_create_pool() → 创建独立内存池
         ├── c->recv = ngx_recv; c->send = ngx_send
         └── ls->handler(c) → HTTP/Stream/Mail 模块接管
```

### 5.2 Read 阶段：非阻塞读请求

HTTP 模块收到连接后，注册读事件到 epoll：

```c
// ngx_http_init_connection() 中:
rev->handler = ngx_http_wait_request_handler;
ngx_handle_read_event(rev, 0);
// → epoll_ctl(EPOLL_CTL_ADD, fd, EPOLLIN|EPOLLET)
```

当 epoll 报告数据到达时：

```c
// ngx_http_wait_request_handler() 中:
n = c->recv(c, buf, size);  // 非阻塞 recv
if (n == NGX_AGAIN) {       // EAGAIN → 数据读完了
    ngx_handle_read_event(rev, 0);  // 重新注册读事件
    return;
}
if (n == 0) {               // 客户端关闭连接
    ngx_close_connection(c);
    return;
}
// 有数据 → 解析 HTTP 请求
```

### 5.3 Write 阶段：非阻塞发送响应

```c
// 响应数据准备好后，注册写事件:
ngx_handle_write_event(wev, lowat);
// → epoll_ctl(EPOLL_CTL_MOD, fd, EPOLLOUT|EPOLLET)

// epoll 报告可写时:
n = c->send(c, buf, len);   // 非阻塞 send
if (n == NGX_AGAIN) {       // EAGAIN → 内核缓冲区满
    ngx_handle_write_event(wev, 0);  // 重新注册写事件
    return;
}
// 数据发送完成
```

### 5.4 非阻塞读写的事件注册/注销

**定义位置**: `src/event/ngx_event.c:267-429`

```c
ngx_int_t
ngx_handle_read_event(ngx_event_t *rev, ngx_uint_t flags)
{
    // ★ ET 模式 (epoll/kqueue):
    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {
        // 事件未激活且未就绪 → 添加到 epoll
        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_CLEAR_EVENT)  // EPOLLET
                == NGX_ERROR)
                return NGX_ERROR;
        }
        return NGX_OK;
    }

    // ★ LT 模式 (select/poll):
    if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {
        if (!rev->active && !rev->ready) {
            // 未活跃 → 添加
            ngx_add_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT);
        }
        if (rev->active && (rev->ready || (flags & NGX_CLOSE_EVENT))) {
            // 已就绪 → 删除(避免反复触发)
            ngx_del_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT | flags);
        }
    }
}
```

**ET vs LT 的区别在此清晰体现**：
- ET 模式：事件只需添加一次，内核只在**状态变化时**通知。`ready=1` 后不需要删除，因为内核不会再通知，除非数据状态再次变化。
- LT 模式：只要缓冲区有数据就通知，所以当数据已经读完（`ready=1`）时必须**删除事件**，否则 epoll_wait 会立即再次返回。

---

## 六、定时器系统 —— 红黑树实现

**源码文件**: `src/event/ngx_event_timer.c`

### 6.1 为什么用红黑树？

定时器需要两个核心操作：
- **找最小超时时间** — O(log n)，作为 epoll_wait 的 timeout 参数
- **插入/删除定时器** — O(log n)，连接频繁设立和取消超时

红黑树完美满足这两个需求，且 Nginx 使用**侵入式设计**——`ngx_rbtree_node_t` 直接嵌入 `ngx_event_t` 结构体，零额外内存分配。

### 6.2 核心操作

```c
// 全局红黑树
ngx_rbtree_t              ngx_event_timer_rbtree;
static ngx_rbtree_node_t  ngx_event_timer_sentinel;  // 哨兵节点

// 初始化
ngx_int_t
ngx_event_timer_init(ngx_log_t *log) {
    ngx_rbtree_init(&ngx_event_timer_rbtree, &ngx_event_timer_sentinel,
                    ngx_rbtree_insert_timer_value);
    return NGX_OK;
}

// ★ 找最近超时时间（红黑树最左叶子节点）
ngx_msec_t
ngx_event_find_timer(void)
{
    ngx_rbtree_node_t  *node, *root, *sentinel;

    if (ngx_event_timer_rbtree.root == &ngx_event_timer_sentinel) {
        return NGX_TIMER_INFINITE;  // 树为空 → 无限等待
    }

    root = ngx_event_timer_rbtree.root;
    sentinel = ngx_event_timer_rbtree.sentinel;
    node = ngx_rbtree_min(root, sentinel);  // ★ 最左节点 = 最小值

    timer = (ngx_msec_int_t) (node->key - ngx_current_msec);
    return (ngx_msec_t) (timer > 0 ? timer : 0);
}

// ★ 处理到期定时器
void
ngx_event_expire_timers(void)
{
    ngx_event_t        *ev;
    ngx_rbtree_node_t  *node, *root, *sentinel;

    sentinel = ngx_event_timer_rbtree.sentinel;

    for ( ;; ) {
        root = ngx_event_timer_rbtree.root;
        if (root == sentinel) return;

        node = ngx_rbtree_min(root, sentinel);

        // 最小 key > 当前时间 → 没有到期定时器
        if ((ngx_msec_int_t) (node->key - ngx_current_msec) > 0) {
            return;
        }

        // ★ 取回事件对象(从嵌入的节点反推)
        ev = ngx_rbtree_data(node, ngx_event_t, timer);

        // 从树中删除
        ngx_rbtree_delete(&ngx_event_timer_rbtree, &ev->timer);

        ev->timer_set = 0;
        ev->timedout = 1;       // ★ 标记超时
        ev->handler(ev);        // ★ 调用回调
    }
}
```

### 6.3 定时器使用示例

```c
// 设置 60 秒超时
ngx_add_timer(rev, 60000);
// 内部: ev->timer.key = ngx_current_msec + 60000
//       ngx_rbtree_insert(&ngx_event_timer_rbtree, &ev->timer)

// 取消超时
ngx_del_timer(rev);

// 在事件处理函数中检查超时:
void my_handler(ngx_event_t *ev) {
    if (ev->timedout) {
        // 超时处理：关闭连接、返回 408 等
        ngx_close_connection(ev->data);
        return;
    }
    // 正常处理...
}
```

---

## 七、完整 HTTP 请求的事件流转

以一个完整的 HTTP 请求为例，展示事件在各环节的流转：

```
═══════════════════════════════════════════════════════════════════════
阶段1: 新连接到达
═══════════════════════════════════════════════════════════════════════
  epoll_wait() 返回: listen fd 可读 (EPOLLIN)
  → rev->handler = ngx_event_accept()
    ├─ accept4(fd, SOCK_NONBLOCK) → 得到 socket fd=42
    ├─ ngx_get_connection(42) → connection 对象
    ├─ ngx_create_pool(4096) → 内存池
    ├─ c->recv = ngx_recv; c->send = ngx_send
    └─ ls->handler(c) → ngx_http_init_connection(c)
         ├─ rev->handler = ngx_http_wait_request_handler
         └─ ngx_handle_read_event(rev, 0)
              └─ epoll_ctl(EPOLL_CTL_ADD, 42, EPOLLIN|EPOLLET)

═══════════════════════════════════════════════════════════════════════
阶段2: 读取 HTTP 请求
═══════════════════════════════════════════════════════════════════════
  epoll_wait() 返回: fd=42 可读 (EPOLLIN)
  → rev->handler = ngx_http_wait_request_handler(rev)
    ├─ n = c->recv(c, buf, size)  // 非阻塞读
    ├─ 循环直到返回 EAGAIN (数据读完)
    ├─ ngx_http_parse_request_line() // 解析 GET /index.html HTTP/1.1
    ├─ ngx_http_parse_header_line()  // 解析 Host: ... 等头部
    ├─ rev->handler = ngx_http_process_request
    └─ ngx_add_timer(rev, 60000)  // 设置 60s 超时

═══════════════════════════════════════════════════════════════════════
阶段3: 处理请求(查找 location, 生成响应)
═══════════════════════════════════════════════════════════════════════
  → ngx_http_process_request(r)
    ├─ ngx_http_handler(r)  // 11 阶段处理器管道
    │   ├─ NGX_HTTP_POST_READ_PHASE
    │   ├─ NGX_HTTP_SERVER_REWRITE_PHASE
    │   ├─ NGX_HTTP_FIND_CONFIG_PHASE   → 匹配 location
    │   ├─ NGX_HTTP_REWRITE_PHASE
    │   ├─ NGX_HTTP_PREACCESS_PHASE
    │   ├─ NGX_HTTP_ACCESS_PHASE         → allow/deny
    │   ├─ NGX_HTTP_PRECONTENT_PHASE
    │   ├─ NGX_HTTP_CONTENT_PHASE        → static/index/proxy
    │   │    └─ (静态文件) ngx_http_static_handler
    │   │       → open/mmap/read 文件
    │   └─ NGX_HTTP_LOG_PHASE            → 写 access log
    │
    ├─ 响应头构建完毕 → ngx_http_write_filter
    └─ ngx_handle_write_event(wev, 0)
         └─ epoll_ctl(EPOLL_CTL_MOD, 42, EPOLLOUT|EPOLLET)

═══════════════════════════════════════════════════════════════════════
阶段4: 发送响应
═══════════════════════════════════════════════════════════════════════
  epoll_wait() 返回: fd=42 可写 (EPOLLOUT)
  → wev->handler = ngx_http_send_response_handler(wev)
    ├─ n = c->send(c, buf, len)  // 非阻塞写
    ├─ 循环直到返回 EAGAIN (内核缓冲区满)
    │   └─ ngx_handle_write_event(wev, 0) // 重新注册写事件
    ├─ 所有数据发送完毕
    └─ 判断是否 keepalive:
        ├─ keepalive → ngx_http_keepalive_handler
        │   └─ ngx_add_timer(rev, keepalive_timeout)
        └─ 否则 → ngx_close_connection(c)
             ├─ ngx_del_event(rev, ...)  // epoll_ctl DEL
             ├─ ngx_del_event(wev, ...)
             ├─ ngx_destroy_pool(c->pool) // ★ 释放整个连接内存
             └─ ngx_free_connection(c)    // 归还连接池

═══════════════════════════════════════════════════════════════════════
阶段5 (可选): 超时处理
═══════════════════════════════════════════════════════════════════════
  ngx_event_expire_timers() 遍历红黑树:
  → ev->timer.key <= ngx_current_msec → 到期!
    ├─ ev->timedout = 1
    ├─ ev->handler(ev) → 检测 timedout 标记
    │   ├─ 读超时 → 返回 408 Request Timeout / 关闭连接
    │   └─ 写超时 → 关闭连接
    └─ ngx_rbtree_delete(红黑树删除)
```

---

## 八、Accept 互斥锁 —— 避免惊群效应

### 8.1 惊群问题

多 worker 进程同时 epoll_wait 同一个 listen fd，一个新连接到达时所有 worker 都被唤醒，但只有一个能 accept 成功，其余白唤醒一次。

### 8.2 Nginx 的解决方案

**定义位置**: `src/event/ngx_event_accept.c:344-379`

```c
ngx_int_t
ngx_trylock_accept_mutex(ngx_cycle_t *cycle)
{
    // ★ 尝试获取共享内存中的互斥锁(非阻塞)
    if (ngx_shmtx_trylock(&ngx_accept_mutex)) {
        // ─── 拿到锁 ───
        // 如果之前没持锁 → 注册 listen fd 的读事件
        if (ngx_accept_mutex_held && ngx_accept_events == 0) {
            return NGX_OK;
        }
        if (ngx_enable_accept_events(cycle) == NGX_ERROR) {
            ngx_shmtx_unlock(&ngx_accept_mutex);
            return NGX_ERROR;
        }
        ngx_accept_events = 0;
        ngx_accept_mutex_held = 1;
        return NGX_OK;
    }

    // ─── 没拿到锁 ───
    if (ngx_accept_mutex_held) {
        // 如果之前持锁 → 注销 listen fd 的读事件(不再参与accept)
        if (ngx_disable_accept_events(cycle, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
        ngx_accept_mutex_held = 0;
    }
    return NGX_OK;
}
```

**流程图**：

```
每轮事件循环:
  ┌──────────────────────────────────┐
  │ ngx_trylock_accept_mutex()       │
  │                                   │
  │  抢到锁? ──YES──→ 注册listen fd    │
  │   │             到自己的 epoll     │
  │   │             flags|=POST_EVENTS │
  │   │                               │
  │   └── NO ────→ 注销listen fd      │
  │               缩短epoll_wait超时  │
  │               尽快重试抢锁        │
  └──────────────────────────────────┘

  + EPOLLEXCLUSIVE (Linux 4.5+) 替代方案:
    所有 worker 都注册 listen fd，但内核保证
    只唤醒一个 worker → 不需要用户态互斥锁
```

---

## 九、连接池管理

### 9.1 预分配

**定义位置**: `src/event/ngx_event.c:635-799 (ngx_event_process_init)`

```c
// 启动时一次性分配所有连接
cycle->connections =
    ngx_alloc(sizeof(ngx_connection_t) * cycle->connection_n, cycle->log);

cycle->read_events =
    ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n, cycle->log);

cycle->write_events =
    ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n, cycle->log);

// 建立空闲连接链表(free_connections)
i = cycle->connection_n;
next = NULL;
do {
    i--;
    c[i].data = next;
    c[i].read = &cycle->read_events[i];
    c[i].write = &cycle->write_events[i];
    c[i].fd = (ngx_socket_t) -1;
    next = &c[i];
} while (i);

cycle->free_connections = next;
cycle->free_connection_n = cycle->connection_n;
```

每个 worker 启动时预分配 `worker_connections` 个连接对象，之后 `ngx_get_connection()` 从链表取，`ngx_free_connection()` 归还链表，全程无 malloc/free。

### 9.2 负载控制

```c
// accept 时计算: 如果空闲连接少于 1/8 → 放弃抢锁
ngx_accept_disabled = ngx_cycle->connection_n / 8
                      - ngx_cycle->free_connection_n;

// 主循环中检查: disabled > 0 时跳过本轮 accept
if (ngx_accept_disabled > 0) {
    ngx_accept_disabled--;
}
```

---

## 十、总结：设计精髓

### 10.1 六个核心设计原则

| # | 原则 | 实现 | 收益 |
|---|------|------|------|
| 1 | **单线程事件循环** | 每个 worker 一个线程 + 一个 epoll 实例 | 零锁竞争，零上下文切换开销 |
| 2 | **全非阻塞 I/O** | accept4(SOCK_NONBLOCK) + 非阻塞 recv/send | 线程永不被阻塞，一个线程管理数万连接 |
| 3 | **ET 边缘触发 + 贪婪读写** | EPOLLET + 读到/写到 EAGAIN | 最小化 epoll_wait 调用次数 |
| 4 | **回调驱动** | `ev->handler` 函数指针 | 事件层和协议层完全解耦 |
| 5 | **红黑树定时器** | 侵入式 `ngx_rbtree_node_t` | O(log n) 超时管理，零额外内存 |
| 6 | **预分配内存池** | 连接池 + 每连接独立 pool | 批量分配/释放，杜绝碎片 |

### 10.2 和传统多线程模型的对比

```
传统多线程(如 Apache prefork):
  ┌──────┐  ┌──────┐  ┌──────┐
  │线程1 │  │线程2 │  │线程3 │  ... 每个连接一个线程
  │recv()│  │recv()│  │recv()│      阻塞 I/O
  │ 阻塞  │  │ 阻塞  │  │ 阻塞  │
  └──────┘  └──────┘  └──────┘
  问题: 1万连接 = 1万线程 = 大量上下文切换 + 内存占用

Nginx:
  ┌──────────────────────────────────────┐
  │            单个 Worker 线程           │
  │                                      │
  │  epoll_wait(10000个fd)               │
  │    ↓                                 │
  │  只有就绪的 fd 才处理                  │
  │  非阻塞 I/O → 永不等一个连接          │
  └──────────────────────────────────────┘
  优势: 1万连接 = 1个线程 = 无上下文切换
```

### 10.3 关键源码索引

| 功能 | 文件 | 行号 |
|------|------|------|
| 事件结构体定义 | `src/event/ngx_event.h` | 30-138 |
| 连接结构体定义 | `src/core/ngx_connection.h` | 127-206 |
| 事件操作接口 | `src/event/ngx_event.h` | 166-183 |
| 事件循环主函数 | `src/event/ngx_event.c` | 194-264 |
| Accept 处理 | `src/event/ngx_event_accept.c` | 20-341 |
| Accept 互斥锁 | `src/event/ngx_event_accept.c` | 344-379 |
| 读事件注册/注销 | `src/event/ngx_event.c` | 267-344 |
| 写事件注册/注销 | `src/event/ngx_event.c` | 347-429 |
| epoll 初始化 | `src/event/modules/ngx_epoll_module.c` | 323-380 |
| epoll 添加事件 | `src/event/modules/ngx_epoll_module.c` | 579-642 |
| epoll 等待分发 | `src/event/modules/ngx_epoll_module.c` | 784-913 |
| 定时器红黑树 | `src/event/ngx_event_timer.c` | 1-126 |
| 连接池预分配 | `src/event/ngx_event.c` | 754-801 |
| HTTP 请求处理阶段 | `src/http/ngx_http_request.c` | (11个阶段定义) |
