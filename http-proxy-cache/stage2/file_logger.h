/*
 * file_logger.h — 文件日志实现
 *
 * 每条日志格式： [2026-05-30 14:30:00] GET http://xxx/ → HIT (1024 bytes)
 * 写入 proxy_access.log
 */

#pragma once

#include "logger_interface.h"
#include <fstream>
#include <string>

class FileLogger : public LoggerInterface {
public:
    explicit FileLogger(const std::string& filepath);
    ~FileLogger();

    void log(const std::string& url,
             const std::string& method,
             const std::string& status,
             size_t bytes) override;

private:
    std::ofstream file_;
    std::string timestamp() const;  // 生成当前时间字符串
};
