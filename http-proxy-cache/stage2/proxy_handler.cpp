/*
 * proxy_handler.cpp — 代理核心逻辑（第二阶段：带LRU缓存+黑白名单）
 *
 * 流程：
 *   收到请求 → 查黑白名单 → 查缓存
 *     ├─ 黑名单命中 → 403 拒绝
 *     ├─ 缓存命中 → 直接返回
 *     └─ 未命中 → 连目标→转发→收响应→存缓存→返回
 */

#include "proxy_handler.h"
#include "common.h"
#include "http_parser.h"
#include "url_parser.h"
#include "socket_util.h"
#include "cache_interface.h"
#include "filter.h"
#include "logger_interface.h"

#include <iostream>
#include <sstream>
#include <unistd.h>
using namespace std;

// ---- 全局缓存 / 过滤实例 ----
// 由 proxy_server.cpp 在启动时通过 set_*() 注入
static CacheInterface*  g_cache  = nullptr;
static Filter*         g_filter = nullptr;
static LoggerInterface* g_logger = nullptr;

void set_cache(CacheInterface* cache)  { g_cache = cache; }
void set_filter(Filter* filter)        { g_filter = filter; }
void set_logger(LoggerInterface* logger) { g_logger = logger; }

CacheInterface* get_cache() { return g_cache; }

// 统计用
static int g_hit_count  = 0;  // 命中次数
static int g_miss_count = 0;  // 未命中次数

// ---- 获取统计信息 ----
int cache_hit_count()  { return g_hit_count; }
int cache_miss_count() { return g_miss_count; }
int cache_hit_rate() {
    int total = g_hit_count + g_miss_count;
    return total == 0 ? 0 : (100 * g_hit_count / total);
}

// ---- 小工具：把原始请求改造成发给目标的格式 ----
static string rewrite_request(const HttpRequest& req, const UrlInfo& info) {
    string result = req.method + " " + info.path + " " + req.version + "\r\n";

    istringstream stream(req.raw);
    string line;
    getline(stream, line);  // 跳过请求行

    while (getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) break;

        if (line.find("Host:") == 0)   continue;
        if (line.find("Proxy-") == 0)  continue;

        result += line + "\r\n";
    }

    result += "Host: " + info.host + "\r\n";
    result += "Connection: close\r\n";
    result += "\r\n";
    return result;
}

// ---- 从目标服务器读取完整响应 ----
static string read_all_response(int target_fd) {
    string data;
    char buf[8192];
    ssize_t n;
    while ((n = read(target_fd, buf, sizeof(buf))) > 0) {
        data.append(buf, n);
    }
    return data;
}

// ---- 主流程 ----

void handle_proxy_request(int browser_fd) {
    // [步骤1] 读浏览器请求
    char buf[8192];
    ssize_t n = read(browser_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    // [步骤2] 解析HTTP请求行
    HttpRequest req = parse_http_request(string(buf));
    if (req.method.empty()) {
        cerr << "[代理] 无法解析HTTP请求" << endl;
        return;
    }

    // [步骤3] 解析URL
    UrlInfo info = parse_url(req.url);

    // ========== 黑白名单检查（在缓存之前） ==========
    if (g_filter && !g_filter->is_allowed(info.host)) {
        cerr << "[代理] 403 拒绝访问: " << info.host << endl;
        string deny = "HTTP/1.1 403 Forbidden\r\n"
                      "Content-Type: text/plain\r\n"
                      "Connection: close\r\n\r\n"
                      "Proxy: access denied by filter\r\n";
        write(browser_fd, deny.c_str(), deny.size());
        if (g_logger) g_logger->log(req.url, req.method, "DENIED", deny.size());
        return;
    }

    // ========== 查缓存 ==========

    string cached_response;
    if (g_cache->get(req.url, cached_response)) {
        // 命中！直接返回，不需要连目标服务器
        g_hit_count++;
        cout << "[代理] ★ 缓存命中 ★ " << req.url
             << " (命中率: " << cache_hit_rate() << "%)" << endl;
        write(browser_fd, cached_response.c_str(), cached_response.size());
        if (g_logger) g_logger->log(req.url, req.method, "HIT", cached_response.size());
        return;  // ← 直接返回，下面的连目标、转发代码都不执行
    }

    g_miss_count++;
    cout << "[代理] 缓存未命中 " << req.url
         << " (命中率: " << cache_hit_rate() << "%)" << endl;

    // ========== 未命中：走完整流程 ==========

    // [步骤4] 连接目标服务器
    int target_fd = connect_to_target(info.host, info.port);
    if (target_fd < 0) {
        string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        write(browser_fd, err.c_str(), err.size());
        return;
    }

    // [步骤5] 改造并转发请求
    string forward = rewrite_request(req, info);
    write(target_fd, forward.c_str(), forward.size());

    // [步骤6] 读取完整响应（先全部读到内存，然后做缓存和回传）
    string response = read_all_response(target_fd);
    close(target_fd);

    // [步骤7] 存入缓存
    g_cache->put(req.url, response);
    cout << "[代理] 已缓存 " << req.url
         << " (缓存条数: " << g_cache->size() << "/" << g_cache->capacity() << ")" << endl;

    // [步骤8] 回传给浏览器
    write(browser_fd, response.c_str(), response.size());
    if (g_logger) g_logger->log(req.url, req.method, "MISS", response.size());
}
