/*
 * proxy_server.cpp — 主入口（第二阶段：带缓存统计）
 *
 * 编译：make
 * 运行：./proxy_server
 */

#include "socket_util.h"
#include "proxy_handler.h"
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
using namespace std;

// proxy_handler.cpp 里声明的统计函数
extern int cache_hit_count();
extern int cache_miss_count();
extern int cache_hit_rate();

int main() {
    int listen_fd = create_listen_socket(8888);
    if (listen_fd < 0) return 1;

    cout << "[代理] 运行中（带LRU缓存），浏览器请设置代理 127.0.0.1:8888" << endl;
    cout << "[代理] 缓存容量: 100 条" << endl;

    while (true) {
        int browser_fd = accept(listen_fd, nullptr, nullptr);
        if (browser_fd < 0) {
            perror("accept");
            continue;
        }

        cout << "\n==== 收到新连接 ====" << endl;
        handle_proxy_request(browser_fd);
        close(browser_fd);
        cout << "==== 统计: 命中 " << cache_hit_count()
             << " / 未命中 " << cache_miss_count()
             << " / 命中率 " << cache_hit_rate() << "% ====" << endl;
    }

    close(listen_fd);
    return 0;
}
