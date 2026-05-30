# HTTP 代理缓存服务器 — 设计讨论与优化记录

> 日期：2026-05-30  
> 记录今天的提问、设计决策与代码优化

---

## 1. 正向代理 vs 反向代理

### 提问："这不是反向代理吗？" → "所以这个项目就是简化的Nginx吗？"

最本质的区别在**请求第一行**：

```http
正向代理（本项目）:   GET http://localhost:9999/ HTTP/1.1   ← 带完整URL
反向代理（Nginx）:    GET / HTTP/1.1                       ← 只有路径
```

正向代理中浏览器知道自己配了代理，`GET`后面写完整URL。反向代理对客户端完全透明。

另一个区别：**正向代理替客户端跑腿，反向代理替服务器挡在前面。**

---

## 2. 缓存策略：为什么用LRU？

### 提问："为什么用LRU策略？" → "也就是说类似优先级调度？"

| 策略 | 规则 | 问题 |
|------|------|------|
| LRU | 踢掉最久没访问的 | 一次性扫描会污染（可接受） |
| LFU | 踢掉访问次数最少的 | 历史热门现在不访问了，死活不淘汰 |
| FIFO | 踢掉最早进来的 | 不管是否热门，先来先走 |

**LRU 假设"最近被访问的，短期内更可能再次被访问"**——这符合上网浏览习惯。

### LRU 就是按访问时间排序的优先级调度

```
刚被访问 → 最高优先级 → 排头部
很久没访问 → 最低优先级 → 排尾部 → 淘汰时从这里踢
```

每次`get()`就刷新一次优先级。

---

## 3. 数据结构：链表 + 哈希表 = O(1)

### 提问："为什么用链表？"

每个数据结构单独用都有致命弱点：

```cpp
// 链表：移动O(1)，但查找必须从头到尾遍历O(n)
// 哈希表：查找O(1)，但没有顺序——不知道谁最久没用
```

**解决方案：让两者指同一个东西**

```cpp
// lru_cache.h 第35-38行
using CacheItem = std::pair<std::string, std::string>;     // 节点存(URL, 响应内容)
using CacheList = std::list<CacheItem>;                    // 链表排顺序
using CacheMap  = std::unordered_map<std::string, CacheList::iterator>;  // 哈希表存指针

CacheList list_;  // 头部=最近使用，尾部=最久未用
CacheMap  map_;   // URL → 链表节点指针
```

### 访问时（get）：O(1) 查找 + O(1) 移到头部

```cpp
// lru_cache.cpp
bool LruCache::get(const std::string& url, std::string& value) {
    auto it = map_.find(url);                    // 哈希表 O(1) 定位
    if (it == map_.end()) return false;

    list_.splice(list_.begin(), list_, it->second);  // O(1) 移到头部，只改指针不动数据
    value = it->second->second;                  // 取出响应内容
    return true;
}
```

### 插入时（put）：O(1) 插入 + O(1) 淘汰

```cpp
// lru_cache.cpp
void LruCache::put(const std::string& url, const std::string& value) {
    auto it = map_.find(url);
    if (it != map_.end()) {
        it->second->second = value;               // 已存在：更新内容
        list_.splice(list_.begin(), list_, it->second);  // 移到头部
        return;
    }

    if (list_.size() >= capacity_)   evict();     // O(1) 淘汰

    list_.emplace_front(url, value);              // O(1) 插到头部
    map_[url] = list_.begin();                     // O(1) 存索引
}
```

### 淘汰时（evict）：O(1)

```cpp
// lru_cache.cpp
void LruCache::evict() {
    auto& back = list_.back();   // 链表尾部 = 最久没用
    map_.erase(back.first);      // O(1) 从哈希表删
    list_.pop_back();            // O(1) 从链表删
}
```

**所有操作都是O(1)，而且`splice`只动指针，不管缓存的HTML是10KB还是10MB，移动都是常数时间。**

---

## 4. 对比Nginx的LRU实现

### 提问："nginx的lru源码和思路是什么样的？" → "所以用红黑树也可以？"

### Nginx源码（ngx_http_file_cache.c）

Nginx缓存节点同时挂在两棵结构上：

```c
// ngx_http_cache.h 第39-62行
typedef struct {
    ngx_rbtree_node_t   node;    // 红黑树节点 → 负责查找（相当于我们的map）
    ngx_queue_t         queue;   // 链表节点 → 负责LRU顺序（相当于我们的list）
    time_t              expire;  // TTL过期时间
    unsigned            count:20; // 引用计数
    unsigned            uses:10;  // 使用次数
    // ...
} ngx_http_file_cache_node_t;
```

**访问时**（第894-979行）：
```c
if (fcn) {
    ngx_queue_remove(&fcn->queue);                       // 先从链表摘下
}
// ... 
fcn->expire = ngx_time() + cache->inactive;
ngx_queue_insert_head(&cache->sh->queue, &fcn->queue);   // 插到头部 = 刚用过
```

**强制淘汰时**（第1791-1812行）：
```c
for ( ;; ) {
    q = ngx_queue_last(&cache->sh->queue);    // 取尾部 = 最久没用
    fcn = ngx_queue_data(q, ...);
    
    if (fcn->count == 0) {                    // 没人正在用
        ngx_http_file_cache_delete(cache, q, name);  // 删掉
        break;
    }
    // 有人用就跳过，移到头部（防止死锁的worker占用）
    ngx_queue_remove(q);
    ngx_queue_insert_head(&cache->sh->queue, &fcn->queue);
}
```

### 我们 vs Nginx

| | 本项目 | Nginx |
|------|--------|-------|
| 查找结构 | `unordered_map`（哈希表） | `ngx_rbtree_t`（红黑树） |
| 顺序结构 | `std::list` | `ngx_queue_t` |
| 查找复杂度 | O(1) | O(log n) |
| 淘汰复杂度 | O(1) | O(1) |

**Nginx用红黑树不用哈希表的原因：** 缓存元数据存在共享内存（多个worker进程共享），哈希表扩容要重新分配共享内存+rehash，在共享内存里做这事极其复杂。红黑树不需要扩容，对共享内存友好。

---

## 5. 磁盘为什么不用红黑树：B+树

### 提问："所以磁盘是一页一页存？" → "B+树索引少层数少怎么做到快速查询？"

```
磁盘最小读写单位是4KB一页。
红黑树一个节点几十字节，散落在不同页，每次找子节点一次随机IO（10ms）。
B+树一个节点塞满4KB（约250个key），3层能查250³ = 1500万条数据，只需3次IO。
```

**一句话：红黑树是瘦高（每节点1个key，磁盘跳很多次），B+树是矮胖（每节点几百个key，很少跳）。**

---

## 6. 优化1：缓存接口化（依赖注入）

### 提问："我建议把缓存单独分离出来，之后可能会拓展功能"

### 改前

```cpp
// proxy_handler.cpp 改前
#include "lru_cache.h"                  // handler 直接依赖 LRU
static LruCache g_cache(100);           // 直接创建具体对象
g_cache.get(url, value);                // 直接调用
g_cache.put(url, response);
```

如果以后要换成TTL缓存，proxy_handler.cpp 必须改代码——这违反了模块分离原则。

### 改后：加缓存接口

```cpp
// cache_interface.h — 抽象接口
class CacheInterface {
public:
    virtual ~CacheInterface() = default;
    virtual bool get(const std::string& url, std::string& value) = 0;
    virtual void put(const std::string& url, const std::string& value) = 0;
    virtual bool contains(const std::string& url) const = 0;
    virtual size_t size() const = 0;
    virtual size_t capacity() const = 0;
};
```

```cpp
// lru_cache.h — 加继承
class LruCache : public CacheInterface { ... };
```

```cpp
// proxy_handler.cpp 改后
#include "cache_interface.h"            // 只依赖接口！不再include lru_cache.h
static CacheInterface* g_cache = nullptr;  // 类型是接口指针

// 注入函数
void set_cache(CacheInterface* cache) {
    g_cache = cache;
}

// 使用时全是虚函数调用
g_cache->get(req.url, cached_response);   // 编译时只知道接口有get
g_cache->put(req.url, response);          // 运行时才找到LruCache的实现
```

```cpp
// proxy_server.cpp — 只有main决定用什么缓存
#include "lru_cache.h"                   // 只有main知道LRU
set_cache(new LruCache(100));            // 以后换TtlCache只改这行
```

**以后加TTL缓存：新建 class TtlCache : public CacheInterface，然后 proxy_server.cpp 里改一行就行。**

---

## 7. 优化2：黑白名单过滤

### 提问："那127是我的电脑里面才有的吗？" → "那内网是什么？" → "为什么阻止不了循环连接？"

### 模块代码

```cpp
// filter.h
class Filter {
public:
    void add_black(const std::string& host);
    void add_white(const std::string& host);
    bool is_allowed(const std::string& host) const;  // true=允许

private:
    bool match_any(const std::string& host, const std::vector<std::string>& rules) const;
    std::vector<std::string> blacklist_;
    std::vector<std::string> whitelist_;
};
```

```cpp
// filter.cpp — 检查逻辑：先黑名单、后白名单
bool Filter::is_allowed(const std::string& host) const {
    if (match_any(host, blacklist_)) return false;        // 黑名单命中 → 拒绝
    if (!whitelist_.empty() && !match_any(host, whitelist_))
        return false;                                      // 白名单启用但不在其中 → 拒绝
    return true;
}

bool Filter::match_any(const std::string& host, const vector<std::string>& rules) const {
    for (const auto& rule : rules)
        if (host.find(rule) != std::string::npos)   // 子串匹配
            return true;
    return false;
}
```

### 注入方式（跟缓存一样）

```cpp
// proxy_server.cpp
Filter* filter = new Filter();
filter->add_black("127.0.0.1");     // 本机回环
filter->add_black("localhost");      // 本机
filter->add_black("192.168.");       // C类内网
filter->add_black("10.");            // A类内网
filter->add_black("172.16.");        // B类内网
// ... 172.17 ~ 172.31
set_filter(filter);
```

### 名单在缓存之前

```cpp
// proxy_handler.cpp — 正确的检查顺序
UrlInfo info = parse_url(req.url);

// ★ 先查名单
if (g_filter && !g_filter->is_allowed(info.host)) {
    write(browser_fd, "403 Forbidden", ...);
    if (g_logger) g_logger->log(req.url, req.method, "DENIED", ...);
    return;  // 直接拒绝，后续缓存和连接跳过
}

// ★ 再查缓存
if (g_cache->get(req.url, cached_response)) { ... }
```

**为什么名单在前：如果先查缓存，黑名单里的URL之前缓存过就会命中返回，黑名单失效。名单是门，缓存是屋——先过大门口的门，再进屋翻抽屉。**

---

## 8. 优化3：文件日志模块

### 代码

```cpp
// logger_interface.h — 日志接口
class LoggerInterface {
public:
    virtual ~LoggerInterface() = default;
    virtual void log(const std::string& url, const std::string& method,
                     const std::string& status, size_t bytes) = 0;
};
```

```cpp
// file_logger.cpp — 追加写入文件
void FileLogger::log(const std::string& url, const std::string& method,
                     const std::string& status, size_t bytes) {
    file_ << "[" << timestamp() << "] "
          << method << " " << url << " → " << status
          << " (" << bytes << " bytes)\n";
    file_.flush();  // 立即落盘，防崩溃丢日志
}
```

### 日志输出效果

```
[2026-05-30 13:37:26] GET http://www.baidu.com/ → MISS (2677 bytes)
[2026-05-30 13:37:26] GET http://www.baidu.com/ → HIT (2677 bytes)
[2026-05-30 13:37:26] GET http://localhost:9999/ → DENIED (103 bytes)
```

三条记录分别对应未命中（连真实服务器）、命中（直接内存返回）、拒绝（黑名单拦截）。

---

## 9. stage2 最终架构图

```
proxy_server.cpp (main — 组装工)
    │
    ├── set_cache(new LruCache(100))               ← 创建缓存
    ├── set_filter(filter)                           ← 创建过滤
    ├── set_logger(new FileLogger("proxy_access.log"))  ← 创建日志
    │
    └── while(true) {
            accept()
            handle_proxy_request(browser_fd)
                │
                ├── HTTP解析 → URL解析
                ├── g_filter->is_allowed()    ← ★ 黑名单检查
                ├── g_cache->get()             ← ★ 缓存查询
                ├── connect_to_target()        ← 连目标
                ├── g_cache->put()             ← ★ 存缓存
                └── g_logger->log()            ← ★ 记日志
        }
```

**同一天加了三个模块，全部通过接口注入，统一的设计模式。**

---

## 10. 测试验证

### 缓存满淘汰测试

```
容量100，发101个不同URL → 第1个被踢 → 再访问第1个是未命中，第101个是命中 ✅
```

### 安全测试

| 测试 | 预期 | 结果 |
|------|------|------|
| 访问 localhost:9999 | 403 拒绝 | 403 ✅ |
| 访问 127.0.0.1 | 403 拒绝 | 403 ✅ |
| 访问 192.168.1.1 | 403 拒绝 | 403 ✅ |
| 访问 www.baidu.com | 正常通过 | 通过 ✅ |

### 缓存命中测试

```
第1次 www.baidu.com → MISS，连百度，存缓存
第2次 www.baidu.com → HIT，不连百度，内存返回 ✅
```
