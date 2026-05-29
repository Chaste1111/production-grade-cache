/*
 * url_parser.h — URL解析模块
 *
 * 把浏览器发来的完整URL拆成 主机 / 端口 / 路径
 *   输入: "http://localhost:9999/api/data?id=1"
 *   输出: host="localhost"  port=9999  path="/api/data?id=1"
 */

#pragma once

#include "common.h"

// 解析URL，失败时port返回-1
UrlInfo parse_url(const std::string& url);
