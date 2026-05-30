/*
 * http_parser.h — HTTP请求解析模块
 *
 * 从TCP数据流中解析出HTTP请求行和方法、URL、版本
 */

#pragma once

#include "common.h"

// 从原始HTTP报文解析请求行，失败返回空结构（method为空）
HttpRequest parse_http_request(const std::string& raw_data);
