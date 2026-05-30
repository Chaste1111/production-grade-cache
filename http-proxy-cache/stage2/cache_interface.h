/*
 * cache_interface.h — 缓存抽象接口
 *
 * 所有缓存实现都继承此类，proxy_handler 只依赖接口
 * 以后加 TTL缓存 / 磁盘缓存 / LFU 只需新增实现
 */

#pragma once

#include <string>
#include <vector>

class CacheInterface {
public:
    virtual ~CacheInterface() = default;

    virtual bool get(const std::string& url, std::string& value) = 0;
    virtual void put(const std::string& url, const std::string& value) = 0;
    virtual bool contains(const std::string& url) const = 0;
    virtual size_t size() const = 0;
    virtual size_t capacity() const = 0;
    virtual std::vector<std::string> all_urls() const = 0;
};
