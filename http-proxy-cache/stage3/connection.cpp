/*
 * connection.cpp — 连接状态机（stage3 核心）
 *
 * 每个 Connection 在 epoll 驱动下，每次推进一小步
 * 7 个状态：READ_REQUEST → CHECKING → CONNECTING → FORWARD
 *                                  → READ_RESPONSE → WRITE_CLIENT → DONE
 *                             缓存命中直接跳到 WRITE_CLIENT
 */

#include "connection.h"
#include "epoller.h"
#include "common.h"
#include "http_parser.h"
#include "url_parser.h"
#include "socket_util.h"
#include "cache_interface.h"
#include "filter.h"
#include "logger_interface.h"
#include "header_mod.h"

#include <iostream>
#include <unistd.h>
using namespace std;

// ==== 外部注入的模块 ====
static CacheInterface*  g_cache  = nullptr;
static Filter*         g_filter = nullptr;
static LoggerInterface* g_logger    = nullptr;
static HeaderMod*       g_header_mod = nullptr;
static int g_hit_count  = 0;
static int g_miss_count = 0;

void set_cache(CacheInterface* c)     { g_cache = c; }
void set_filter(Filter* f)            { g_filter = f; }
void set_logger(LoggerInterface* l)   { g_logger = l; }
void set_header_mod(HeaderMod* h)     { g_header_mod = h; }

// ==== 前向声明 ====
static void handle_read_request(Connection* c, Epoller* ep);
static void handle_checking(Connection* c, Epoller* ep);
static void handle_connecting(Connection* c, Epoller* ep);
static void handle_forward(Connection* c, Epoller* ep);
static void handle_read_response(Connection* c, Epoller* ep);
static void handle_write_client(Connection* c, Epoller* ep);
static void handle_tunnel(Connection* c, Epoller* ep);
static void handle_done(Connection* c, Epoller* ep);

// ==== 入口 ====
void process_connection(Connection* c, Epoller* ep) {
    switch (c->state) {
        case State::READ_REQUEST:   handle_read_request(c, ep);   break;
        case State::CHECKING:       handle_checking(c, ep);       break;
        case State::CONNECTING:     handle_connecting(c, ep);     break;
        case State::FORWARD:        handle_forward(c, ep);        break;
        case State::READ_RESPONSE:  handle_read_response(c, ep);  break;
        case State::WRITE_CLIENT:   handle_write_client(c, ep);   break;
        case State::TUNNEL:         handle_tunnel(c, ep);         break;
        case State::DONE:           handle_done(c, ep);           break;
    }
}

// ============================================================
//  状态1: READ_REQUEST — 从浏览器收请求
// ============================================================
static void handle_read_request(Connection* c, Epoller* ep) {
    char buf[4096];
    ssize_t n = read(c->browser_fd, buf, sizeof(buf) - 1);

    if (n > 0) { buf[n] = '\0'; c->in_buf += buf; }

    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        c->state = State::DONE; handle_done(c, ep); return;
    }

    if (c->is_request_complete()) {
        HttpRequest req = parse_http_request(c->in_buf);
        UrlInfo info   = parse_url(req.url);
        c->method = req.method;  c->url = req.url;  c->version = req.version;
        c->host   = info.host;   c->port = info.port; c->path = info.path;

        cout << "[conn " << c->browser_fd << "] " << c->method
             << " " << c->host << ":" << c->port << c->path << endl;

        c->state = State::CHECKING;
        handle_checking(c, ep);
    }
    // 没读完 → 返回 epoll_wait 继续等
}

// ============================================================
//  状态2: CHECKING — 查黑名单 → 查缓存 → 决定下一步
// ============================================================
static void handle_checking(Connection* c, Epoller* ep) {
    // --- HTTPS CONNECT 隧道 ---
    if (c->method == "CONNECT") {
        // CONNECT 的 URL 是 "host:port"，不是完整URL
        size_t colon = c->url.find(':');
        c->host = c->url.substr(0, colon);
        c->port = (colon != string::npos) ? stoi(c->url.substr(colon + 1)) : 443;
        c->path = "";

        cout << "[conn " << c->browser_fd << "] CONNECT 隧道 → "
             << c->host << ":" << c->port << endl;

        c->target_fd = connect_to_target(c->host, c->port);
        if (c->target_fd < 0) {
            string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            c->out_buf = err; c->write_offset = 0;
            c->state = State::WRITE_CLIENT;
            ep->mod(c->browser_fd, EPOLLOUT, c);
            handle_write_client(c, ep); return;
        }
        c->state = State::CONNECTING;
        ep->add(c->target_fd, EPOLLOUT, c);
        return;
    }

    // --- 2a. 黑名单 ---
    if (g_filter && !g_filter->is_allowed(c->host)) {
        cerr << "[conn " << c->browser_fd << "] 403 拒绝: " << c->host << endl;
        c->out_buf = "HTTP/1.1 403 Forbidden\r\n"
                     "Content-Type: text/plain\r\n"
                     "Connection: close\r\n\r\n"
                     "Proxy: access denied by filter\r\n";
        c->write_offset = 0;  c->state = State::WRITE_CLIENT;
        ep->mod(c->browser_fd, EPOLLOUT, c);
        handle_write_client(c, ep);  return;
    }

    // --- 2b. 缓存 ---
    string cached;
    if (g_cache && g_cache->get(c->url, cached)) {
        g_hit_count++;  c->cache_hit = true;
        cout << "[conn " << c->browser_fd << "] ★ 缓存命中 ★ " << c->url << endl;
        c->out_buf = cached;  c->write_offset = 0;
        c->state = State::WRITE_CLIENT;
        ep->mod(c->browser_fd, EPOLLOUT, c);
        handle_write_client(c, ep);  return;
    }

    // --- 2c. 未命中：连目标 ---
    g_miss_count++;
    cout << "[conn " << c->browser_fd << "] 缓存未命中 " << c->url << endl;

    c->target_fd = connect_to_target(c->host, c->port);
    if (c->target_fd < 0) {
        c->out_buf = "HTTP/1.1 502 Bad Gateway\r\n"
                     "Content-Type: text/plain\r\n"
                     "Connection: close\r\n\r\n"
                     "Proxy: cannot reach target\r\n";
        c->write_offset = 0;  c->state = State::WRITE_CLIENT;
        ep->mod(c->browser_fd, EPOLLOUT, c);
        handle_write_client(c, ep);  return;
    }

    c->state = State::CONNECTING;
    ep->add(c->target_fd, EPOLLOUT, c);   // 等握手完成
}

// ============================================================
//  状态3: CONNECTING — TCP握手完成，改请求，转发
// ============================================================
static void handle_connecting(Connection* c, Epoller* ep) {
    cout << "[conn " << c->browser_fd << "] TCP握手完成 (target="
         << c->target_fd << ")" << endl;

    // HTTPS CONNECT: 告诉浏览器隧道已建立，进入双向中继
    if (c->method == "CONNECT") {
        string ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
        write(c->browser_fd, ok.c_str(), ok.size());
        cout << "[conn " << c->browser_fd << "] 隧道已建立，开始双向中继" << endl;

        c->state = State::TUNNEL;
        // 两个fd都盯着 EPOLLIN，哪个有数据就中继到另一个
        ep->mod(c->browser_fd, EPOLLIN, c);
        ep->mod(c->target_fd,  EPOLLIN, c);
        return;
    }

    // 改造请求：
    // 1. 应用头修改规则（add/remove/replace）
    // 2. GET http://host/path → GET /path
    // 3. 修正 Host 头

    string modified;
    if (g_header_mod) {
        modified = g_header_mod->apply(c->in_buf);
    } else {
        modified = c->in_buf;
    }

    // 把请求行里的完整URL替换成路径
    string original_line = c->method + " " + c->url + " " + c->version;
    string new_line      = c->method + " " + c->path + " " + c->version;

    size_t pos = modified.find(original_line);
    if (pos != string::npos) {
        modified.replace(pos, original_line.size(), new_line);
    }

    // 修正 Host 头（可能被 header_mod 改了）
    size_t host_pos = modified.find("Host: ");
    size_t host_end = modified.find("\r\n", host_pos);
    if (host_pos != string::npos && host_end != string::npos) {
        modified.replace(host_pos, host_end - host_pos,
                         "Host: " + c->host);
    }

    c->out_buf = modified;  c->write_offset = 0;
    c->state = State::FORWARD;
    ep->mod(c->target_fd, EPOLLOUT, c);
    handle_forward(c, ep);
}

// ============================================================
//  状态4: FORWARD — 把请求发给目标
// ============================================================
static void handle_forward(Connection* c, Epoller* ep) {
    ssize_t n = write(c->target_fd,
                      c->out_buf.c_str() + c->write_offset,
                      c->out_buf.size() - c->write_offset);

    if (n < 0 && errno == EAGAIN) return;  // 等下次 EPOLLOUT

    if (n <= 0) { c->state = State::DONE; handle_done(c, ep); return; }

    c->write_offset += n;
    if (c->write_offset >= c->out_buf.size()) {
        c->out_buf.clear();
        c->state = State::READ_RESPONSE;
        ep->mod(c->target_fd, EPOLLIN, c);  // 改盯可读
    }
}

// ============================================================
//  状态5: READ_RESPONSE — 从目标收响应（可能分多次）
// ============================================================
static void handle_read_response(Connection* c, Epoller* ep) {
    char buf[8192];
    ssize_t n = read(c->target_fd, buf, sizeof(buf));

    if (n > 0) { c->out_buf.append(buf, n); return; }  // 还有更多

    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        // 目标关连接 = 收完了
        cout << "[conn " << c->browser_fd << "] 目标响应收完 ("
             << c->out_buf.size() << " bytes)" << endl;

        if (g_cache)  g_cache->put(c->url, c->out_buf);
        if (g_logger) g_logger->log(c->url, c->method, "MISS", c->out_buf.size());

        ep->del(c->target_fd); close(c->target_fd); c->target_fd = -1;

        c->write_offset = 0;  c->state = State::WRITE_CLIENT;
        ep->mod(c->browser_fd, EPOLLOUT, c);
        handle_write_client(c, ep);
    }
}

// ============================================================
//  状态6: WRITE_CLIENT — 把响应写回浏览器
// ============================================================
static void handle_write_client(Connection* c, Epoller* ep) {
    ssize_t n = write(c->browser_fd,
                      c->out_buf.c_str() + c->write_offset,
                      c->out_buf.size() - c->write_offset);

    if (n < 0 && errno == EAGAIN) return;

    if (n <= 0) { c->state = State::DONE; handle_done(c, ep); return; }

    c->write_offset += n;
    if (c->write_offset >= c->out_buf.size()) {
        if (c->cache_hit && g_logger)
            g_logger->log(c->url, c->method, "HIT", c->out_buf.size());

        c->state = State::DONE;
        handle_done(c, ep);
    }
}

// ============================================================
//  状态7: TUNNEL — HTTPS 双向中继
// ============================================================
static void handle_tunnel(Connection* c, Epoller* ep) {
    char buf[8192];
    ssize_t n;

    // 浏览器 → 目标
    n = read(c->browser_fd, buf, sizeof(buf));
    if (n > 0) {
        write(c->target_fd, buf, n);
    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        // 浏览器断开 → 关目标
        ep->del(c->target_fd); close(c->target_fd);
        c->state = State::DONE; handle_done(c, ep); return;
    }

    // 目标 → 浏览器
    n = read(c->target_fd, buf, sizeof(buf));
    if (n > 0) {
        write(c->browser_fd, buf, n);
    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        // 目标断开 → 关浏览器
        c->state = State::DONE; handle_done(c, ep); return;
    }

    // 都没断开 → 继续等下一次 epoll 通知
}

// ============================================================
//  状态8: DONE — 清理，释放内存
// ============================================================
static void handle_done(Connection* c, Epoller* ep) {
    cout << "[conn " << c->browser_fd << "] 关闭" << endl;

    if (c->browser_fd >= 0) { ep->del(c->browser_fd); close(c->browser_fd); }
    if (c->target_fd  >= 0) { ep->del(c->target_fd);  close(c->target_fd);  }

    delete c;
}
