/*
 * proxy_server.cpp — 主入口（Stage3: epoll 非阻塞 + 状态机）
 *
 * 双端口 + epoll 事件循环
 *   8888 → 代理端口
 *   8890 → 统计面板
 */

#include "socket_util.h"
#include "epoller.h"
#include "connection.h"
#include "lru_cache.h"
#include "filter.h"
#include "file_logger.h"
#include "header_mod.h"
#include "cache_interface.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <ctime>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace std;

static time_t g_start_time;

// ==== 统计面板 ====
static void handle_stats(int client_fd) {
    ostringstream html;
    html << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
         << "Connection: close\r\n\r\n"
         << "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"UTF-8\">"
         << "<meta http-equiv=\"refresh\" content=\"3\">"
         << "<title>代理面板 (epoll)</title></head><body>"
         << "<h1>📊 HTTP代理 (Stage3 — epoll)</h1>"
         << "<p>启动: " << ctime(&g_start_time) << "</p>"
         << "<p>端口: 8888(代理) | 8890(面板)</p>"
         << "<p><b>状态:</b> 事件驱动 | 非阻塞IO | epoll</p>"
         << "</body></html>";

    string page = html.str();
    write(client_fd, page.c_str(), page.size());
    close(client_fd);
}

// ==== 主循环 ====
int main() {
    g_start_time = time(nullptr);

    // 组装模块
    set_cache(new LruCache(100, 300));  // 100条, 5分钟TTL

    // 黑名单：屏蔽内网地址段
    Filter* filter = new Filter();
    filter->add_black("192.168.");
    filter->add_black("10.");
    filter->add_black("172.16.");
    set_filter(filter);

    set_logger(new FileLogger("proxy_access.log"));

    // 请求头修改规则
    HeaderMod* hmod = new HeaderMod();
    hmod->add_rule("add", "X-Proxy-Server", "http-proxy-cache/3.0");
    hmod->add_rule("add", "X-Forwarded-For", "client");
    set_header_mod(hmod);

    // 创建监听 socket（非阻塞）
    int proxy_fd = create_listen_socket(8888);
    int stats_fd = create_listen_socket(8890);
    if (proxy_fd < 0 || stats_fd < 0) return 1;

    // 创建 epoll
    Epoller ep;

    // 注册两个监听 fd
    ep.add(proxy_fd, EPOLLIN, (void*)(intptr_t)proxy_fd);
    ep.add(stats_fd, EPOLLIN, (void*)(intptr_t)stats_fd);

    cout << "═══════════════════════════════════" << endl;
    cout << "  Stage3 — epoll 非阻塞代理" << endl;
    cout << "  代理: 8888  |  面板: 8890" << endl;
    cout << "═══════════════════════════════════" << endl;

    while (true) {
        auto events = ep.wait(-1);   // epoll_wait 睡觉等事件

        for (auto& ev : events) {
            // 用 fd 判断是哪个端口
            int fd = (int)(intptr_t)ev.data.ptr;

            if (fd == proxy_fd || fd == stats_fd) {
                // 新连接
                int client = accept(fd, nullptr, nullptr);
                if (client < 0) {
                    if (errno == EAGAIN) continue;  // 非阻塞，没有新连接
                    continue;
                }

                // 设为非阻塞
                fcntl(client, F_SETFL, O_NONBLOCK);

                if (fd == stats_fd) {
                    // 统计面板请求
                    cout << "[主] 面板被访问" << endl;
                    handle_stats(client);
                    // 面板请求简单，直接处理完关闭
                } else {
                    // 代理请求：创建 Connection
                    Connection* c = new Connection();
                    c->browser_fd = client;
                    c->state = State::READ_REQUEST;
                    ep.add(client, EPOLLIN, c);  // 注册到epoll，绑Connection指针
                    cout << "[主] 新连接 fd=" << client << endl;
                }
            } else {
                // 已有连接来数据了 → Connection* 绑在 ptr 里
                Connection* c = (Connection*)ev.data.ptr;
                process_connection(c, &ep);
            }
        }
    }

    close(proxy_fd);
    close(stats_fd);
    return 0;
}
