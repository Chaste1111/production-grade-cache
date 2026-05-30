/*
 * connection.h — 连接状态机
 *
 * 每个 Connection 代表一次完整的代理请求
 * 包含浏览器fd、目标fd、当前状态、收发缓冲区
 */

#pragma once

#include <string>

class Epoller;
class CacheInterface;
class Filter;
class LoggerInterface;
class HeaderMod;

// 连接状态：一次代理请求的生命周期
enum class State {
    READ_REQUEST,      // 正在从浏览器读 HTTP 请求
    CHECKING,          // 请求收完了，检查黑名单 + 缓存
    CONNECTING,        // 正在连目标服务器（非阻塞 connect）
    FORWARD,           // 正在把请求发给目标
    READ_RESPONSE,     // 正在从目标读响应
    WRITE_CLIENT,      // 正在把响应写回浏览器
    TUNNEL,            // HTTPS CONNECT 隧道 — 双向中继
    DONE               // 完成，等待清理
};

struct Connection {
    // ---- socket ----
    int browser_fd = -1;   // 与浏览器的连接
    int target_fd  = -1;   // 与目标服务器的连接（缓存命中时不创建）

    // ---- 状态 ----
    State state = State::READ_REQUEST;

    // ---- 缓冲区 ----
    std::string in_buf;     // 从浏览器读到的数据（累加，直到收到完整 \r\n\r\n）
    std::string out_buf;    // 要发回浏览器的数据（从目标读到的响应）
    size_t      write_offset = 0;  // out_buf 已经写到了哪个位置

    // ---- 解析结果 ----
    std::string method;     // GET / POST
    std::string url;        // 原始 URL，如 http://localhost:9999/
    std::string version;    // HTTP/1.1
    std::string host;       // 目标主机
    int         port = 80;  // 目标端口
    std::string path;       // 请求路径

    // ---- 缓存命中标记 ----
    bool cache_hit = false;

    // ---- 辅助方法 ----
    bool is_request_complete() const {
        // HTTP 请求头以 \r\n\r\n 结束
        return in_buf.find("\r\n\r\n") != std::string::npos;
    }
};

// ==== 公开的函数 ====
struct epoll_event;  // 前向声明，避免 include epoll.h

// epoll 通知有事件时调用，根据 c->state 推进状态机
void process_connection(Connection* c, Epoller* ep);

// 注入外部模块
void set_cache(CacheInterface* c);
void set_filter(Filter* f);
void set_logger(LoggerInterface* l);
void set_header_mod(HeaderMod* h);

CacheInterface* get_cache();  // 给面板用
