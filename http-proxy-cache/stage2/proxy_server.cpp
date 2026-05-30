/*
 * proxy_server.cpp — 主入口（第二阶段：双端口 + select + 统计面板）
 *
 * 8888 → 代理端口（缓存 + 黑白名单）
 * 8890 → 统计面板（返回HTML管理页面）
 *
 * 编译：make
 * 运行：./proxy_server
 */

#include "socket_util.h"
#include "proxy_handler.h"
#include "lru_cache.h"
#include "filter.h"
#include "file_logger.h"
#include "cache_interface.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <ctime>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace std;

// ---- 读取日志文件最后 N 行 ----
static string read_last_lines(const string& filepath, int n) {
    ifstream file(filepath);
    if (!file) return "日志文件不可用";

    vector<string> lines;
    string line;
    while (getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    // 取最后 N 行
    int start = max(0, (int)lines.size() - n);
    string result;
    for (int i = start; i < (int)lines.size(); i++)
        result += lines[i] + "\n";
    return result.empty() ? "暂无日志" : result;
}

// ---- 全局启动时间 ----
static time_t g_start_time;

// ---- 统计面板：生成HTML并返回给浏览器 ----
static void handle_stats_request(int client_fd) {
    CacheInterface* cache = get_cache();

    ostringstream html;
    html << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/html; charset=utf-8\r\n"
         << "Connection: close\r\n\r\n"

         << "<!DOCTYPE html>\n"
         << "<html lang=\"zh-CN\">\n<head>\n"
         << "<meta charset=\"UTF-8\">\n"
         << "<meta http-equiv=\"refresh\" content=\"3\">\n"  // 3秒自动刷新
         << "<title>代理服务器 — 运行状态</title>\n"
         << "<style>\n"
         << "  body{font-family:Arial,sans-serif;max-width:700px;"
            "margin:40px auto;background:#f5f5f5}\n"
         << "  .card{background:#fff;border-radius:8px;padding:20px;"
            "margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,.1)}\n"
         << "  h1{color:#333;margin-top:0}\n"
         << "  .hit{color:#4caf50;font-weight:bold}\n"
         << "  .miss{color:#ff9800;font-weight:bold}\n"
         << "  .denied{color:#f44336;font-weight:bold}\n"
         << "  table{width:100%;border-collapse:collapse}\n"
         << "  td,th{padding:8px 12px;border-bottom:1px solid #eee;text-align:left}\n"
         << "  .bar{height:20px;border-radius:4px;background:#eee;overflow:hidden}\n"
         << "  .bar div{height:100%;float:left;transition:width .3s}\n"
         << "  .bar-hit{background:#4caf50}\n"
         << "  .bar-miss{background:#ff9800}\n"
         << "  .bar-deny{background:#f44336}\n"
         << "</style>\n</head>\n<body>\n"

         << "<h1>📊 HTTP代理缓存服务器</h1>\n"

         // 运行信息
         << "<div class=\"card\">\n"
         << "<h2>运行状态</h2>\n"
         << "<p>启动时间: " << ctime(&g_start_time) << "</p>\n"
         << "<p>代理端口: <b>8888</b> &nbsp;|&nbsp; 管理端口: <b>8890</b></p>\n"
         << "</div>\n"

         // 请求统计
         << "<div class=\"card\">\n"
         << "<h2>请求统计</h2>\n";

    int hit = cache_hit_count(), miss = cache_miss_count();
    int total = hit + miss;
    if (total > 0) {
        int hit_pct  = 100 * hit  / total;
        int miss_pct = 100 - hit_pct;

        html << "<div class=\"bar\">\n"
             << "<div class=\"bar-hit\" style=\"width:" << hit_pct  << "%\"></div>\n"
             << "<div class=\"bar-miss\" style=\"width:" << miss_pct << "%\"></div>\n"
             << "</div>\n";
    }

    html << "<table>\n"
         << "<tr><td>请求总数</td><td><b>" << total << "</b></td></tr>\n"
         << "<tr><td class=\"hit\">缓存命中</td><td><b>" << hit
         << " (" << cache_hit_rate() << "%)</b></td></tr>\n"
         << "<tr><td class=\"miss\">缓存未命中</td><td><b>" << miss << "</b></td></tr>\n"
         << "</table>\n"
         << "</div>\n"

         // 缓存详情
         << "<div class=\"card\">\n"
         << "<h2>缓存状态</h2>\n";

    if (cache) {
        html << "<p>已缓存: <b>" << cache->size() << " / " << cache->capacity()
             << "</b> 条</p>\n";

        vector<string> urls = cache->all_urls();
        if (!urls.empty()) {
            html << "<table>\n"
                 << "<tr><th>#</th><th>URL（越靠上=越近访问）</th></tr>\n";
            int i = 1;
            for (const auto& url : urls) {
                html << "<tr><td>" << i++ << "</td><td>"
                     << (url.size() > 80 ? url.substr(0, 77) + "..." : url)
                     << "</td></tr>\n";
            }
            html << "</table>\n";
        } else {
            html << "<p>暂无缓存</p>\n";
        }
    } else {
        html << "<p>缓存未启用</p>\n";
    }
    html << "</div>\n"

         // 最近日志
         << "<div class=\"card\">\n"
         << "<h2>📋 最近日志（最后20条）</h2>\n"
         << "<pre style=\"background:#fafafa;padding:12px;border-radius:4px;"
            "overflow-x:auto;font-size:13px;line-height:1.6;max-height:400px;"
            "overflow-y:auto;white-space:pre-wrap;word-break:break-all\">"
         << read_last_lines("proxy_access.log", 20)
         << "</pre>\n"
         << "</div>\n"
         << "</body>\n</html>\n";

    string page = html.str();
    write(client_fd, page.c_str(), page.size());
    close(client_fd);
}

// ---- 主函数 ----
int main() {
    g_start_time = time(nullptr);

    // 组装各个模块（全部接口化注入）
    set_cache(new LruCache(100));

    Filter* filter = new Filter();
    filter->add_black("127.0.0.1");
    filter->add_black("localhost");
    filter->add_black("192.168.");
    filter->add_black("10.");
    filter->add_black("172.16."); filter->add_black("172.17."); filter->add_black("172.18.");
    filter->add_black("172.19."); filter->add_black("172.20."); filter->add_black("172.21.");
    filter->add_black("172.22."); filter->add_black("172.23."); filter->add_black("172.24.");
    filter->add_black("172.25."); filter->add_black("172.26."); filter->add_black("172.27.");
    filter->add_black("172.28."); filter->add_black("172.29."); filter->add_black("172.30.");
    filter->add_black("172.31.");
    set_filter(filter);

    set_logger(new FileLogger("proxy_access.log"));

    // 创建两个监听端口
    int proxy_fd = create_listen_socket(8888);
    int stats_fd = create_listen_socket(8890);
    if (proxy_fd < 0 || stats_fd < 0) return 1;

    int maxfd = max(proxy_fd, stats_fd);

    cout << "══════════════════════════════════════" << endl;
    cout << "  HTTP代理缓存服务器 v2.1" << endl;
    cout << "  代理端口: 8888  (浏览器设置代理 127.0.0.1:8888)" << endl;
    cout << "  管理面板: 8890  (浏览器访问 http://localhost:8890/)" << endl;
    cout << "══════════════════════════════════════" << endl;

    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(proxy_fd, &read_set);
        FD_SET(stats_fd, &read_set);

        // 同时盯两个端口，哪个有数据就处理哪个
        int ready = select(maxfd + 1, &read_set, nullptr, nullptr, nullptr);
        if (ready < 0) {
            perror("select");
            continue;
        }

        // 代理端口有连接
        if (FD_ISSET(proxy_fd, &read_set)) {
            int browser_fd = accept(proxy_fd, nullptr, nullptr);
            if (browser_fd >= 0) {
                cout << "\n==== 收到代理请求 ====" << endl;
                handle_proxy_request(browser_fd);
                close(browser_fd);
                cout << "==== 命中 " << cache_hit_count()
                     << " / 未命中 " << cache_miss_count()
                     << " / 命中率 " << cache_hit_rate() << "% ====" << endl;
            }
        }

        // 统计面板有连接
        if (FD_ISSET(stats_fd, &read_set)) {
            int stats_client = accept(stats_fd, nullptr, nullptr);
            if (stats_client >= 0) {
                cout << "[管理] 统计面板被访问" << endl;
                handle_stats_request(stats_client);
            }
        }
    }

    close(proxy_fd);
    close(stats_fd);
    return 0;
}
