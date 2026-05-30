/*
 * epoller.cpp — epoll 封装实现
 */

#include "epoller.h"
#include <iostream>
#include <unistd.h>
using namespace std;

Epoller::Epoller() {
    epfd_ = epoll_create(1);
    if (epfd_ < 0) {
        perror("epoll_create");
        exit(1);
    }
    cout << "[epoll] 监控室已创建 (epfd=" << epfd_ << ")" << endl;
}

Epoller::~Epoller() {
    close(epfd_);
}

void Epoller::add(int fd, uint32_t event_type, void* ptr) {
    epoll_event ev{};
    ev.events   = event_type;
    ev.data.ptr = ptr;                        // 绑定的指针，wait 回来时原样返回
    epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    cout << "[epoll] ADD fd=" << fd
         << (event_type & EPOLLIN  ? " IN"  : "")
         << (event_type & EPOLLOUT ? " OUT" : "") << endl;
}

void Epoller::mod(int fd, uint32_t event_type, void* ptr) {
    epoll_event ev{};
    ev.events   = event_type;
    ev.data.ptr = ptr;
    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    cout << "[epoll] MOD fd=" << fd
         << (event_type & EPOLLIN  ? " IN"  : "")
         << (event_type & EPOLLOUT ? " OUT" : "") << endl;
}

void Epoller::del(int fd) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    cout << "[epoll] DEL fd=" << fd << endl;
}

vector<epoll_event> Epoller::wait(int timeout_ms) {
    const int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    int n = epoll_wait(epfd_, events, MAX_EVENTS, timeout_ms);
    if (n < 0) {
        perror("epoll_wait");
        return {};
    }

    // 把就绪的事件装进 vector 返回
    return vector<epoll_event>(events, events + n);
}
