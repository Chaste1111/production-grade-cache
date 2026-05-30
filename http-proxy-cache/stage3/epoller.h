/*
 * epoller.h — epoll 封装
 *
 * 三个系统调用的薄封装：
 *   epoll_create → 建监控室
 *   epoll_ctl    → 往监控室加/删/改 fd
 *   epoll_wait   → 睡觉等事件，醒来拿就绪列表
 */

#pragma once

#include <sys/epoll.h>
#include <vector>

class Epoller {
public:
    Epoller();
    ~Epoller();

    // 把 fd 加进来，盯 event_type (EPOLLIN / EPOLLOUT)
    // ptr: 绑一个指针，事件就绪时原样返回（我们绑 Connection*）
    void add(int fd, uint32_t event_type, void* ptr);

    // 修改已注册 fd 的盯梢类型
    void mod(int fd, uint32_t event_type, void* ptr);

    // 把 fd 从监控室移除
    void del(int fd);

    // 睡觉，等事件。timeout_ms=-1 表示永远等
    // 返回就绪的事件数组
    std::vector<epoll_event> wait(int timeout_ms = -1);

private:
    int epfd_;  // epoll 实例的 fd
};
