/*
 * file_logger.cpp — 文件日志实现
 */

#include "file_logger.h"
#include <ctime>
#include <iomanip>
#include <iostream>

FileLogger::FileLogger(const std::string& filepath) {
    file_.open(filepath, std::ios::app);  // 追加模式
    if (!file_) {
        std::cerr << "[日志] 无法打开日志文件: " << filepath << std::endl;
    }
    file_ << "\n===== 代理启动 =====\n";
    file_.flush();
}

FileLogger::~FileLogger() {
    file_ << "===== 代理关闭 =====\n";
}

void FileLogger::log(const std::string& url,
                     const std::string& method,
                     const std::string& status,
                     size_t bytes) {
    if (!file_) return;

    file_ << "[" << timestamp() << "] "
          << method << " " << url << " → " << status
          << " (" << bytes << " bytes)\n";
    file_.flush();  // 立即写入磁盘，防崩溃丢日志
}

std::string FileLogger::timestamp() const {
    auto now = std::time(nullptr);
    auto* tm = std::localtime(&now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    return buf;
}
