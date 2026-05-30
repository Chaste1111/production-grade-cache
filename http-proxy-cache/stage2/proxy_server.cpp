/*
 * proxy_server.cpp — 主入口（第二阶段：带缓存统计）
 *
 * 编译：make
 * 运行：./proxy_server
 */

#include "socket_util.h"
#include "proxy_handler.h"
#include "lru_cache.h"
#include "filter.h"
#include "file_logger.h"
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
using namespace std;

int main() {
    // 选择缓存实现（以后换 TtlCache 只改这一行）
    set_cache(new LruCache(100));

    // 设置黑名单：屏蔽内网和本机
    Filter* filter = new Filter();
    filter->add_black("127.0.0.1");
    filter->add_black("localhost");
    filter->add_black("192.168.");
    filter->add_black("10.");
    filter->add_black("172.16.");
    filter->add_black("172.17.");
    filter->add_black("172.18.");
    filter->add_black("172.19.");
    filter->add_black("172.20.");
    filter->add_black("172.21.");
    filter->add_black("172.22.");
    filter->add_black("172.23.");
    filter->add_black("172.24.");
    filter->add_black("172.25.");
    filter->add_black("172.26.");
    filter->add_black("172.27.");
    filter->add_black("172.28.");
    filter->add_black("172.29.");
    filter->add_black("172.30.");
    filter->add_black("172.31.");
    set_filter(filter);

    // 设置文件日志
    set_logger(new FileLogger("proxy_access.log"));

    int listen_fd = create_listen_socket(8888);
    if (listen_fd < 0) return 1;

    cout << "[代理] 运行中（带LRU缓存），浏览器请设置代理 127.0.0.1:8888" << endl;

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
