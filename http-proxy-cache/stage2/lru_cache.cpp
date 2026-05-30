/*
 * lru_cache.cpp — LRU缓存实现
 *
 * 核心思路：
 *   get("url")
 *     1. 哈希表 O(1) 找到节点
 *     2. 把该节点移到链表头部（标记为"刚用过"）
 *
 *   put("url", "data")
 *     1. 如果已存在：更新内容，移到头部
 *     2. 如果不存在：插到头部
 *     3. 如果满了：删掉链表尾部的节点（最久没用的）
 */

#include "lru_cache.h"
#include <cassert>

LruCache::LruCache(size_t capacity) : capacity_(capacity) {
    assert(capacity > 0);
}

bool LruCache::get(const std::string& url, std::string& value) {
    auto it = map_.find(url);
    if (it == map_.end())
        return false;  // 没找到

    // 移到链表头部（表示刚被访问）
    list_.splice(list_.begin(), list_, it->second);
    value = it->second->second;  // it->second 是链表迭代器 → 取 pair 的 second = 响应内容
    return true;
}

void LruCache::put(const std::string& url, const std::string& value) {
    auto it = map_.find(url);
    if (it != map_.end()) {
        // 已存在：更新内容，移到头部
        it->second->second = value;
        list_.splice(list_.begin(), list_, it->second);
        return;
    }

    // 不存在：检查容量
    if (list_.size() >= capacity_)
        evict();

    // 插到链表头部
    list_.emplace_front(url, value);
    map_[url] = list_.begin();
}

bool LruCache::contains(const std::string& url) const {
    return map_.find(url) != map_.end();
}

size_t LruCache::size() const    { return list_.size(); }
size_t LruCache::capacity() const { return capacity_; }

void LruCache::evict() {
    // 链表尾部 = 最久没访问的
    auto& back = list_.back();
    map_.erase(back.first);  // 从哈希表删掉
    list_.pop_back();         // 从链表删掉
}

std::vector<std::string> LruCache::all_urls() const {
    std::vector<std::string> urls;
    // 链表头部=最近使用的，从新到旧遍历
    for (const auto& item : list_)
        urls.push_back(item.first);  // item.first = URL
    return urls;
}
