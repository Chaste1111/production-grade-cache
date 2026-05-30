/*
 * lru_cache.h — LRU缓存模块
 *
 * 用双向链表 + 哈希表实现 O(1) 的查找和淘汰
 *   链表：按访问时间排序，最近用的在头部，最久没用的在尾部
 *   哈希表：URL → 链表节点，O(1) 查找
 */

#pragma once

#include "cache_interface.h"
#include <string>
#include <list>
#include <unordered_map>
#include <ctime>

class LruCache : public CacheInterface {
public:
    // capacity: 最多缓存条数
    // ttl_sec: 每条缓存的有效期（秒），0 = 永不过期
    explicit LruCache(size_t capacity, time_t ttl_sec = 0);

    bool get(const std::string& url, std::string& value) override;
    void put(const std::string& url, const std::string& value) override;
    bool contains(const std::string& url) const override;
    size_t size() const override;
    size_t capacity() const override;
    std::vector<std::string> all_urls() const override;

private:
    struct CacheEntry {
        std::string url;
        std::string response;
        time_t      expire_time;  // 0 = 永不过期
    };

    using CacheList = std::list<CacheEntry>;
    using CacheMap  = std::unordered_map<std::string, CacheList::iterator>;

    void evict();       // LRU 淘汰
    bool expired(const CacheEntry& e) const;  // 是否已过期

    size_t capacity_;
    time_t ttl_sec_;   // 0 = 永不过期
    CacheList list_;
    CacheMap  map_;
};
