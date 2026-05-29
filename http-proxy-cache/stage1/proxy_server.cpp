/*
 * proxy_server.cpp — 主入口
 *
 * 启动代理服务器，循环等待浏览器连接
 *
 * 编译：make
 * 运行：./proxy_server
 * 测试：curl -x http://localhost:8888 http://localhost:9999/
 */

#include "socket_util.h"
#include "proxy_handler.h"
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
using namespace std;

int main() {
    int listen_fd = create_listen_socket(8888);
    if (listen_fd < 0) return 1;

    cout << "[代理] 运行中，浏览器请设置代理 127.0.0.1:8888" << endl;

    while (true) {
        // 等待浏览器连接
        int browser_fd = accept(listen_fd, nullptr, nullptr);
        if (browser_fd < 0) {
            perror("accept");
            continue;
        }

        cout << "\n==== 收到新连接 ====" << endl;

        // 处理这次代理请求（此时阻塞，处理完才接下一个）
        handle_proxy_request(browser_fd);

        close(browser_fd);
        cout << "==== 连接关闭 ====" << endl;
    }

    close(listen_fd);
    return 0;
}
