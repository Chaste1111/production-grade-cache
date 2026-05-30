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

class LruCache : public CacheInterface {
public:
    // capacity: 最多缓存多少条响应
    explicit LruCache(size_t capacity);

    // 查缓存，找到返回 true，value 存响应内容
    bool get(const std::string& url, std::string& value);

    // 存缓存，如果满了就淘汰最久没用的一条
    void put(const std::string& url, const std::string& value);

    // 缓存里有没有这个 URL
    bool contains(const std::string& url) const;

    // 统计信息
    size_t size() const;                      // 当前条数
    size_t capacity() const;                  // 容量上限
    std::vector<std::string> all_urls() const; // 列出所有URL

private:
    // 链表的每个节点存 (url, 响应内容)
    using CacheItem = std::pair<std::string, std::string>;
    using CacheList = std::list<CacheItem>;
    // 哈希表的 value 是指向链表节点的迭代器
    using CacheMap  = std::unordered_map<std::string, CacheList::iterator>;

    void evict();  // 淘汰最久没用的

    size_t    capacity_;
    CacheList list_;  // 头部=最近使用，尾部=最久未用
    CacheMap  map_;
};
