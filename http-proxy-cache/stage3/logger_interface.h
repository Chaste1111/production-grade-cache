/*
 * logger_interface.h — 日志抽象接口
 *
 * 所有日志实现都继承此类，proxy_handler 只依赖接口
 */

#pragma once

#include <string>

class LoggerInterface {
public:
    virtual ~LoggerInterface() = default;

    // 记录一条代理访问日志
    //   url      : 请求的完整URL
    //   method   : GET / POST / ...
    //   status   : HIT（命中缓存）/ MISS（未命中）/ DENIED（被过滤）
    //   bytes    : 返回的字节数
    virtual void log(const std::string& url,
                     const std::string& method,
                     const std::string& status,
                     size_t bytes) = 0;
};
