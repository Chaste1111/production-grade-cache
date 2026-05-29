/*
 * proxy_handler.cpp — 代理核心逻辑实现
 *
 * 一次代理请求的完整流程：
 *   1. 读浏览器请求
 *   2. 解析HTTP → 提取URL
 *   3. 解析URL → 得到主机/端口/路径
 *   4. 连接目标服务器
 *   5. 改造请求（URL改成相对路径）
 *   6. 转发给目标
 *   7. 目标响应 → 回传给浏览器
 */

#include "proxy_handler.h"
#include "common.h"
#include "http_parser.h"
#include "url_parser.h"
#include "socket_util.h"

#include <iostream>
#include <sstream>
#include <unistd.h>
using namespace std;

// ---- 小工具：把原始请求改造成发给目标的格式 ----
//
//   浏览器发给代理的:  GET http://localhost:9999/abc HTTP/1.1
//                      Host: localhost:9999
//                      User-Agent: curl/...
//
//   代理发给目标的:    GET /abc HTTP/1.1
//                      User-Agent: curl/...
//                      Host: localhost
//                      Connection: close
//
static string rewrite_request(const HttpRequest& req, const UrlInfo& info) {
    // 第一行：完整URL → 相对路径
    string result = req.method + " " + info.path + " " + req.version + "\r\n";

    // 逐行处理头部
    istringstream stream(req.raw);
    string line;
    getline(stream, line);  // 跳过请求行

    while (getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) break;  // 空行 = 头部结束

        // 跳过需要替换的头部
        if (line.find("Host:") == 0)   continue;
        if (line.find("Proxy-") == 0)  continue;

        result += line + "\r\n";
    }

    // 加上正确的头
    result += "Host: " + info.host + "\r\n";
    result += "Connection: close\r\n";
    result += "\r\n";
    return result;
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
    cout << "[代理] " << req.method << " " << info.host
         << ":" << info.port << info.path << endl;

    // [步骤4] 连接目标服务器
    int target_fd = connect_to_target(info.host, info.port);
    if (target_fd < 0) {
        string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        write(browser_fd, err.c_str(), err.size());
        return;
    }

    // [步骤5] 改造请求
    string forward = rewrite_request(req, info);

    // [步骤6] 发送给目标
    write(target_fd, forward.c_str(), forward.size());

    // [步骤7] 目标响应 → 回传给浏览器
    char resp_buf[8192];
    ssize_t total = 0;
    while ((n = read(target_fd, resp_buf, sizeof(resp_buf))) > 0) {
        write(browser_fd, resp_buf, n);
        total += n;
    }
    cout << "[代理] 回传完成，共 " << total << " 字节" << endl;

    close(target_fd);
}
