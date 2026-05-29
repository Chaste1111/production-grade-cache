/*
 * common.h — 公共数据结构
 *
 * 所有模块共享的类型定义放在这里，避免循环依赖
 */

#pragma once

#include <string>

// 解析后的URL信息
struct UrlInfo {
    std::string host;   // 目标主机，如 "localhost"
    int         port;   // 目标端口，如 9999（HTTP默认80）
    std::string path;   // 请求路径，如 "/api/data"
};

// 解析后的HTTP请求
struct HttpRequest {
    std::string method;   // GET / POST / CONNECT ...
    std::string url;      // 原始URL（浏览器给的完整地址）
    std::string version;  // HTTP/1.1
    std::string raw;      // 原始报文全文
};
