/*
 * lru_cache.cpp — LRU + TTL 缓存实现
 *
 * 在 LRU 基础上加了 TTL 过期：
 *   get() 时如果条目已过期 → 淘汰 + 返回未命中
 *   ttl_sec=0 表示永不过期（纯 LRU 模式）
 */

#include "lru_cache.h"
#include <cassert>

LruCache::LruCache(size_t capacity, time_t ttl_sec)
    : capacity_(capacity), ttl_sec_(ttl_sec) {
    assert(capacity > 0);
}

bool LruCache::get(const std::string& url, std::string& value) {
    auto it = map_.find(url);
    if (it == map_.end())
        return false;

    CacheEntry& entry = *it->second;

    // 检查 TTL 过期
    if (expired(entry)) {
        auto list_it = it->second;    // 先取出链表迭代器
        map_.erase(it);                // 再从哈希表删
        list_.erase(list_it);          // 最后从链表删
        return false;
    }

    // 移到头部（刚被访问）
    list_.splice(list_.begin(), list_, it->second);
    value = entry.response;
    return true;
}

void LruCache::put(const std::string& url, const std::string& value) {
    auto it = map_.find(url);
    if (it != map_.end()) {
        // 已存在：更新内容和过期时间，移到头部
        CacheEntry& entry = *it->second;
        entry.response = value;
        entry.expire_time = ttl_sec_ ? time(nullptr) + ttl_sec_ : 0;
        list_.splice(list_.begin(), list_, it->second);
        return;
    }

    // 满了就淘汰
    if (list_.size() >= capacity_)
        evict();

    // 插到头部
    CacheEntry entry;
    entry.url         = url;
    entry.response    = value;
    entry.expire_time = ttl_sec_ ? time(nullptr) + ttl_sec_ : 0;

    list_.emplace_front(std::move(entry));
    map_[url] = list_.begin();
}

bool LruCache::contains(const std::string& url) const {
    auto it = map_.find(url);
    if (it == map_.end()) return false;
    return !expired(*it->second);  // 过期当作不存在
}

size_t LruCache::size() const    { return list_.size(); }
size_t LruCache::capacity() const { return capacity_; }

std::vector<std::string> LruCache::all_urls() const {
    std::vector<std::string> urls;
    for (const auto& entry : list_)
        if (!expired(entry))
            urls.push_back(entry.url);
    return urls;
}

void LruCache::evict() {
    auto& back = list_.back();
    map_.erase(back.url);
    list_.pop_back();
}

bool LruCache::expired(const CacheEntry& e) const {
    if (ttl_sec_ == 0) return false;                // 永不过期
    return e.expire_time != 0 && time(nullptr) > e.expire_time;
}
