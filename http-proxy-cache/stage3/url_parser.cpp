/*
 * url_parser.cpp — URL解析实现
 */

#include "url_parser.h"
using namespace std;

UrlInfo parse_url(const string& url) {
    UrlInfo info;
    info.port = 80;  // HTTP 默认端口

    string u = url;

    // 去掉 "http://" 前缀（7个字符）
    if (u.find("http://") == 0)
        u = u.substr(7);

    // 找第一个 '/' —— 分隔主机和路径
    size_t slash = u.find('/');
    string host_part;
    if (slash != string::npos) {
        host_part = u.substr(0, slash);
        info.path = u.substr(slash);   // 保留 / 及之后所有内容
    } else {
        host_part = u;
        info.path = "/";
    }

    // host_part 可能是 "localhost:9999"，拆出主机名和端口
    size_t colon = host_part.find(':');
    if (colon != string::npos) {
        info.host = host_part.substr(0, colon);
        info.port = stoi(host_part.substr(colon + 1));
    } else {
        info.host = host_part;
    }

    return info;
}
