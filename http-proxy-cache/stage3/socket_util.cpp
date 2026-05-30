/*
 * socket_util.cpp — Socket工具实现
 */

#include "socket_util.h"
#include <iostream>
#include <cstring>
#include <fcntl.h>       // O_NONBLOCK 定义在这里
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
using namespace std;

int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // 端口复用，防止重启报 "Address already in use"
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    listen(fd, 10);

    // 设为非阻塞 —— epoll 要求所有 fd 都不能阻塞
    fcntl(fd, F_SETFL, O_NONBLOCK);

    cout << "[socket] 监听端口 " << port << " (非阻塞)" << endl;
    return fd;
}

int connect_to_target(const string& host, int port) {
    // DNS解析：域名 → IP地址
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        cerr << "[socket] DNS解析失败: " << host << endl;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // 设非阻塞 — connect 立刻返回，不等 TCP 握手完成
    fcntl(fd, F_SETFL, O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {   // 不是"正在进行中"→真的错误
            perror("connect");
            close(fd);
            return -1;
        }
        // EINPROGRESS = TCP握手在后台进行，正常
    }

    cout << "[socket] 正在连接 " << host << ":" << port
         << " (fd=" << fd << ", 等待 epoll EPOLLOUT)" << endl;
    return fd;
}
