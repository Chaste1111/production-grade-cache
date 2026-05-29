/*
 * http_parser.cpp — HTTP请求解析实现
 */

#include "http_parser.h"
#include <sstream>
using namespace std;

HttpRequest parse_http_request(const string& raw_data) {
    HttpRequest req;
    req.raw = raw_data;

    istringstream stream(raw_data);
    string line;
    if (!getline(stream, line))
        return req;  // 空数据，返回空结构

    // 去掉行尾的 \r
    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    // 请求行格式：GET http://localhost:9999/ HTTP/1.1
    istringstream line_stream(line);
    line_stream >> req.method >> req.url >> req.version;

    return req;
}
