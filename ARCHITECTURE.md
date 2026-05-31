# 生产级缓存架构设计文档

> **项目名称**: production-grade-cache  
> **业务场景**: 电商商品详情页 — QPS 10,000，读多写少  
> **设计目标**: 高并发、高可用、多层缓存、解耦、可观测

---

## 目录

- [1. 系统全景](#1-系统全景)
- [2. 网络拓扑与安全隔离](#2-网络拓扑与安全隔离)
- [3. Nginx 接入层](#3-nginx-接入层)
- [4. Java 应用层](#4-java-应用层)
- [5. Redis 缓存层](#5-redis-缓存层)
- [6. MySQL 存储层](#6-mysql-存储层)
- [7. 缓存核心问题与防护](#7-缓存核心问题与防护)
- [8. 多环境配置管理](#8-多环境配置管理)
- [9. 故障处理与降级策略](#9-故障处理与降级策略)
- [10. 监控、告警与可观测性](#10-监控告警与可观测性)
- [11. 安全体系](#11-安全体系)
- [12. 部署方案](#12-部署方案)
- [13. 启动检查清单](#13-启动检查清单)

---

## 1. 系统全景

```
                              ┌──────────────┐
                              │   浏览器      │
                              └──────┬───────┘
                                     │ HTTPS
                              ┌──────▼───────┐
                              │  DNS 解析     │
                              └──────┬───────┘
                                     │
══════════════════════════════════════╪═══════════════════════════
         公网                        │                    公网
══════════════════════════════════════╪═══════════════════════════
         内网                        │                    内网
                              ┌──────▼───────┐
                              │   Nginx      │  :80/:443
                              │  (反向代理)   │  限流 + 日志 + gzip
                              └──────┬───────┘
                                     │ 内网 HTTP
                              ┌──────▼───────┐
                              │  Java 应用    │  :9000
                              │  Spring Boot  │  Caffeine L1 缓存
                              │  + Tomcat     │  HikariCP 连接池
                              └──┬────────┬──┘
                                 │        │
                          ┌──────▼──┐ ┌──▼──────┐
                          │  Redis  │ │  MySQL   │
                          │  :6379  │ │  :3306   │
                          │  L2缓存  │ │  持久化   │
                          └─────────┘ └──────────┘
                                 │        │
                          ┌──────▼──┐ ┌──▼──────┐
                          │ Sentinel│ │  Slave  │
                          │ 哨兵模式 │ │  只读从库│
                          └─────────┘ └──────────┘
```

### 请求生命周期

```
请求: GET /product/1001

① Nginx (10ms)
   ├─ 限流检查（令牌桶）
   ├─ gzip 压缩
   └─ proxy_pass → Java 应用

② Java 应用
   ├─ Caffeine L1 缓存命中 → 直接返回（纳秒级）
   ├─ L1 未命中 → Redis L2 缓存（毫秒级）
   │    ├─ 命中 → 回填 L1 → 返回
   │    └─ 未命中 → MySQL（毫秒~秒级）
   │         ├─ 查主库或从库
   │         ├─ 回填 Redis
   │         └─ 回填 Caffeine
   └─ 返回 JSON

③ 响应沿原路返回
```

---

## 2. 网络拓扑与安全隔离

### 2.1 三层隔离

```
┌─────────────────────────────────────────────────────────────┐
│  公网区域 (DMZ)                                              │
│  ├─ Nginx: 公网IP → 对外暴露 80/443 端口                       │
│  └─ 除此之外，没有任何东西对公网开放                              │
└─────────────────────────────────────────────────────────────┘
                              │ 防火墙
┌─────────────────────────────────────────────────────────────┐
│  应用内网 (10.0.1.0/24)                                      │
│  ├─ Java 应用 × N 台: 只有内网 IP，端口 9000                    │
│  ├─ 应用之间通过内网通信                                       │
│  └─ 对公网不可见                                              │
└─────────────────────────────────────────────────────────────┘
                              │ 防火墙 / 安全组
┌─────────────────────────────────────────────────────────────┐
│  数据内网 (10.0.2.0/24)                                      │
│  ├─ Redis: 只监听内网 IP 10.0.2.10                            │
│  ├─ MySQL: 只监听内网 IP 10.0.2.20                            │
│  └─ 安全组: 仅允许 10.0.1.0/24 的应用网段访问                    │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 实际手段

| 层 | 手段 | 配置/命令 |
|----|------|----------|
| 网络 | 防火墙/安全组 | 只开放 10.0.1.0/24 → 10.0.2.20:3306 |
| MySQL | `bind-address` | `10.0.2.20`（绝不 0.0.0.0） |
| Redis | `bind` | `10.0.2.10` |
| 应用 | 环境变量 | 密码不入配置文件，由 K8s Secret 注入 |

### 2.3 开发者如何访问生产数据库

```
开发者本地 ─SSH隧道→ 跳板机(堡垒机) ─内网→ MySQL 10.0.2.20:3306

# SSH 端口转发
ssh -L 3307:10.0.2.20:3306 user@bastion.example.com
# 然后本地连 127.0.0.1:3307
```

**绝不直接在本地用公网 IP 连生产数据库。**

---

## 3. Nginx 接入层

### 3.1 核心配置

```nginx
# /etc/nginx/nginx.conf

# === 全局层 ===
user www-data;
worker_processes auto;          # CPU 核数
worker_connections 4096;        # 每个 worker 最大连接数
multi_accept on;                # 一次 accept 所有新连接

# === events 层 ===
events {
    use epoll;                  # Linux epoll（PPT CH07核心）
    worker_connections 4096;
}

# === HTTP 层 ===
http {
    # 基础优化
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 65;
    keepalive_requests 100;

    # gzip 压缩（减少带宽）
    gzip on;
    gzip_min_length 1024;
    gzip_types application/json text/plain;

    # === 限流（令牌桶算法）===
    # 每个 IP 每秒最多 50 请求，突发 20
    limit_req_zone $binary_remote_addr zone=per_ip:10m rate=50r/s;
    limit_req_zone $server_name zone=per_server:10m rate=10000r/s;

    # 连接数限制
    limit_conn_zone $binary_remote_addr zone=conn_per_ip:10m;

    # === 上游（Java 应用集群）===
    upstream java_backend {
        least_conn;                     # 最少连接数调度
        server 10.0.1.11:9000 weight=1 max_fails=3 fail_timeout=30s;
        server 10.0.1.12:9000 weight=1 max_fails=3 fail_timeout=30s;
        server 10.0.1.13:9000 weight=1 max_fails=3 fail_timeout=30s;

        keepalive 64;                   # 到后端的 keepalive 连接数
    }

    # === 虚拟主机（生产站点）===
    server {
        listen 80;
        server_name api.shop.com;

        # 限流
        limit_req zone=per_ip burst=20 nodelay;
        limit_conn conn_per_ip 10;

        # 超时配置
        proxy_connect_timeout 3s;       # 连后端超时
        proxy_read_timeout 3s;          # 读后端响应超时
        proxy_send_timeout 3s;

        # 请求头透传
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;

        # 代理到 Java 应用
        location / {
            proxy_pass http://java_backend;
        }

        # 健康检查
        location /health {
            proxy_pass http://java_backend/actuator/health;
        }
    }
}
```

### 3.2 Nginx 兜底

| 问题 | 配置 |
|------|------|
| 后端全挂了 | `proxy_next_upstream error timeout http_502;` 逐个重试 |
| 某台机器慢 | `fail_timeout=30s` 标记不可用，30 秒后重新尝试 |
| 单 IP 刷接口 | `limit_req` + `limit_conn` 双限流 |
| 大文件吃带宽 | `proxy_buffering on` + 限制 buffer 大小 |

---

## 4. Java 应用层

### 4.1 分层架构

```
┌─────────────────────────────────────────┐
│  Controller 层                           │
│  - 接收HTTP请求                          │
│  - 参数校验 (@Valid)                     │
│  - 调用 Service                          │
│  - 统一返回 Result<T>                    │
└────────────────┬────────────────────────┘
                 │
┌────────────────▼────────────────────────┐
│  Service 层                              │
│  - 业务逻辑                              │
│  - 缓存策略（先查缓存再查库）               │
│  - 事务管理 (@Transactional)             │
│  - 缓存更新（删缓存/更新缓存）              │
└────────┬───────────────────┬─────────────┘
         │                   │
┌────────▼────────┐  ┌───────▼──────────┐
│  Cache 模块      │  │  Repository 层    │
│  - Caffeine L1  │  │  - MyBatis Mapper │
│  - Redis L2     │  │  - SQL 查询       │
│  - 缓存策略      │  │  - 读写路由       │
└─────────────────┘  └──────────────────┘
```

### 4.2 连接池精确配置（PPT CH08 落地）

```yaml
# application-prod.yml
spring:
  datasource:
    # === 主库（写）===
    master:
      jdbc-url: jdbc:mysql://10.0.2.20:3306/shop?useSSL=true
      username: ${DB_MASTER_USER}
      password: ${DB_MASTER_PASSWORD}
      hikari:
        maximum-pool-size: 20        # 估算: (CPU核数*2) + 磁盘数
        minimum-idle: 5              # 闲时保留最少连接
        idle-timeout: 600000         # 空闲 10 分钟回收
        connection-timeout: 3000     # 等待连接最多 3 秒
        max-lifetime: 1800000        # 连接最长活 30 分钟
        leak-detection-threshold: 10000  # 连接泄露检测（10秒）
        validation-timeout: 1000

    # === 从库（读）===
    slave:
      jdbc-url: jdbc:mysql://10.0.2.21:3306/shop?useSSL=true
      username: ${DB_SLAVE_USER}
      password: ${DB_SLAVE_PASSWORD}
      hikari:
        maximum-pool-size: 20
        minimum-idle: 5
        read-only: true             # 只读连接，防误写
```

### 4.3 读写分离路由（PPT CH08 落地）

```java
@Component
public class DataSourceRouter implements DataSourceRoutingKey {

    @Override
    public String determineCurrentLookupKey() {
        // 从 ThreadLocal 读取当前操作类型
        String type = DataSourceContextHolder.getDataSourceType();
        if ("READ".equals(type)) return "slave";   // 读走从库
        return "master";                            // 写走主库，默认主库
    }
}

// 使用时
@Transactional
public void createProduct(Product p) {
    DataSourceContextHolder.set("WRITE");   // 写主库
    productMapper.insert(p);
}

@Cacheable("products")
public Product getProduct(Long id) {
    DataSourceContextHolder.set("READ");    // 读从库
    return productMapper.selectById(id);
}
```

### 4.4 本地缓存 Caffeine（PPT CH08 落地）

```java
@Configuration
public class CacheConfig {

    // === L1: Caffeine 本地缓存（纳秒级） ===
    @Bean
    public Cache<Long, Product> productLocalCache() {
        return Caffeine.newBuilder()
            .maximumSize(10_000)                      // 最多 1 万条
            .expireAfterWrite(1, TimeUnit.MINUTES)    // 写入 1 分钟后过期
            .expireAfterAccess(30, TimeUnit.SECONDS)  // 30 秒无访问过期
            .recordStats()                            // 统计命中率
            .build();
    }

    // === L2: Redis 分布式缓存（毫秒级） ===
    @Bean
    public RedisTemplate<String, Product> productRedisTemplate(
            RedisConnectionFactory factory) {
        RedisTemplate<String, Product> template = new RedisTemplate<>();
        template.setConnectionFactory(factory);
        template.setValueSerializer(new GenericJackson2JsonRedisSerializer());
        return template;
    }
}
```

### 4.5 多级缓存查询逻辑

```java
@Service
public class ProductService {

    private final Cache<Long, Product> localCache;       // L1
    private final RedisTemplate<String, Product> redis;  // L2
    private final ProductMapper productMapper;           // DB

    public Product getProduct(Long id) {
        // === ① L1: Caffeine 本地缓存（最快）===
        Product product = localCache.getIfPresent(id);
        if (product != null) {
            metrics.recordL1Hit();
            return product;
        }
        metrics.recordL1Miss();

        // === ② L2: Redis 分布式缓存 ===
        String cacheKey = "product:" + id;
        String cached = redis.opsForValue().get(cacheKey);
        if (cached != null) {
            product = JSON.parseObject(cached, Product.class);
            localCache.put(id, product);   // 回填 L1
            metrics.recordL2Hit();
            return product;
        }
        metrics.recordL2Miss();

        // === ③ L3: MySQL ===
        product = productMapper.selectById(id);
        if (product != null) {
            // 回填 L2 + L1
            redis.opsForValue().set(cacheKey, JSON.toJSONString(product),
                5 + ThreadLocalRandom.current().nextInt(60),  // 随机抖动
                TimeUnit.MINUTES);
            localCache.put(id, product);
        }
        return product;
    }
}
```

### 4.6 线程池配置

```yaml
spring:
  task:
    execution:
      pool:
        core-size: 8            # CPU 密集型 → CPU核数+1
        max-size: 16
        queue-capacity: 200     # 排队上限
      thread-name-prefix: shop-
```

### 4.7 优雅启停

```java
@Component
public class GracefulShutdown implements ApplicationListener<ContextClosedEvent> {

    @Override
    public void onApplicationEvent(ContextClosedEvent event) {
        log.warn("收到关闭信号，开始优雅停机...");
        // 1. 停止接收新请求（Tomcat 已内置）
        // 2. 等待正在处理的请求完成（最多 30 秒）
        // 3. 关闭连接池
        // 4. 关闭 Redis 连接
        // 5. 关闭线程池
        log.warn("优雅停机完成");
    }
}
```

```yaml
server:
  shutdown: graceful
spring:
  lifecycle:
    timeout-per-shutdown-phase: 30s
```

---

## 5. Redis 缓存层

### 5.1 部署拓扑（哨兵模式）

```
┌──────────────────────────────────────┐
│  Sentinel 哨兵集群（3节点，多数派选举）  │
│  sentinel-1 :26379                   │
│  sentinel-2 :26379                   │
│  sentinel-3 :26379                   │
└──────────────┬───────────────────────┘
               │ 监控 + 自动故障转移
┌──────────────▼───────────────────────┐
│  Redis 主从                           │
│  redis-master :6379  ← 读写           │
│  redis-slave-1 :6379  ← 只读          │
│  redis-slave-2 :6379  ← 只读          │
└──────────────────────────────────────┘
```

### 5.2 Java 连接配置

```yaml
spring:
  redis:
    # === 哨兵模式 ===
    sentinel:
      master: mymaster
      nodes:
        - 10.0.2.11:26379
        - 10.0.2.12:26379
        - 10.0.2.13:26379
    # === Lettuce 连接池 ===
    lettuce:
      pool:
        max-active: 16
        max-idle: 8
        min-idle: 4
        max-wait: 2000ms
    timeout: 3000ms
```

### 5.3 数据结构选型（PPT CH08 落地）

| 业务数据 | Redis 类型 | Key 设计 | TTL |
|----------|-----------|----------|-----|
| 商品基本信息 | **String** (JSON) | `product:{id}` | 5分钟 + 随机抖动 |
| 商品价格 | **String** | `product:{id}:price` | 1分钟 |
| 商品评价列表 | **List** | `product:{id}:reviews` | 30分钟 |
| 商品标签 | **Set** | `product:{id}:tags` | 1小时 |
| 热销排行 | **ZSet** | `products:rank` | 当天 |
| 用户最近浏览 | **List** (LTRIM 保留 20 条) | `user:{id}:history` | 7天 |

### 5.4 持久化策略

```conf
# redis.conf

# RDB（全量快照，快速恢复）
save 900 1       # 15 分钟内至少 1 个 key 变化
save 300 10      # 5 分钟内至少 10 个 key 变化
save 60 10000    # 1 分钟内至少 10000 个 key 变化

# AOF（增量日志，数据安全）
appendonly yes
appendfsync everysec   # 每秒刷盘，折中方案

# 淘汰策略
maxmemory-policy allkeys-lru   # 内存满了淘汰最久未用的
```

---

## 6. MySQL 存储层

### 6.1 账号权限分离（PPT CH08 落地）

```sql
-- === 管理员：全部权限（仅 DBA 持有）===
CREATE USER 'admin'@'10.0.2.%' IDENTIFIED BY '高强度随机密码';
GRANT ALL PRIVILEGES ON *.* TO 'admin'@'10.0.2.%';

-- === 应用账号：只有业务库 CRUD（应用代码使用）===
CREATE USER 'app_user'@'10.0.1.%' IDENTIFIED BY '高强度随机密码';
GRANT SELECT, INSERT, UPDATE, DELETE ON shop.* TO 'app_user'@'10.0.1.%';

-- === 只读账号：数据分析/报表 ===
CREATE USER 'read_user'@'10.0.1.%' IDENTIFIED BY '高强度随机密码';
GRANT SELECT ON shop.* TO 'read_user'@'10.0.1.%';

-- === 备份账号：SELECT + LOCK TABLES ===
CREATE USER 'backup'@'10.0.2.%' IDENTIFIED BY '高强度随机密码';
GRANT SELECT, LOCK TABLES ON *.* TO 'backup'@'10.0.2.%';
```

### 6.2 慢查询监控（PPT CH08 落地）

```sql
-- MySQL 配置
SET GLOBAL slow_query_log = ON;
SET GLOBAL long_query_time = 0.1;        -- 100ms 以上记录
SET GLOBAL log_queries_not_using_indexes = ON;
SET GLOBAL min_examined_row_limit = 1000;
```

### 6.3 索引规范

```sql
-- ✅ 好的索引
CREATE INDEX idx_product_cat_status ON products(category_id, status);
-- category_id 等值在前，status 等值在后

-- ✅ 覆盖索引（SQL 只用索引就完成，不回表）
CREATE INDEX idx_cover ON products(id, name, price, stock);

-- ✅ 联合索引最左前缀原则
-- INDEX(a, b, c)  ->  能用: WHERE a=?  /  WHERE a=? AND b=?  /  WHERE a=? AND c=?
--                      不能用: WHERE b=?  /  WHERE c=?

-- ❌ 坏的做法
-- 不要 SELECT *
-- 不要在索引列上用函数: WHERE YEAR(create_time) = 2026
-- 不要 LIKE '%xxx'（前置通配符）
-- 不要在大表上直接 ALTER TABLE（用 pt-online-schema-change）
```

### 6.4 备份策略

```bash
#!/bin/bash
# /opt/scripts/db_backup.sh

# 从从库备份，不影响主库性能
SLAVE_HOST="10.0.2.21"
BACKUP_USER="backup"
BACKUP_PASS=$(cat /etc/mysql/backup.cnf | grep password | cut -d= -f2)
BACKUP_FILE="/backup/shop_$(date +%Y%m%d_%H%M).sql.gz"

mysqldump \
    -h ${SLAVE_HOST} \       # 从库上备份（PPT CH08）
    -u ${BACKUP_USER} \       # backup 账号（PPT CH08 权限分离）
    -p${BACKUP_PASS} \
    --single-transaction \    # 不锁表备份（InnoDB）
    --master-data=2 \
    --routines --triggers \
    shop | gzip > ${BACKUP_FILE}

# 只保留最近 30 天的备份
find /backup/ -name "shop_*.sql.gz" -mtime +30 -delete

# 上传到对象存储（异地容灾）
aws s3 cp ${BACKUP_FILE} s3://backup-bucket/mysql/
```

```bash
# crontab: 每天凌晨 3:00 执行
0 3 * * * /opt/scripts/db_backup.sh >> /var/log/db_backup.log 2>&1
```

---

## 7. 缓存核心问题与防护

### 7.1 缓存穿透防护

**问题**: 大量请求查询不存在的数据，直接穿透到底层。

```java
// === 方案：布隆过滤器 + 空值缓存 + 参数校验 ===

@Service
public class CachePenetrationGuard {

    private final BloomFilter<Long> productIdFilter;  // 布隆过滤器
    private final RedisTemplate<String, String> redis;

    public Product getProduct(Long id) {
        // ① 参数校验：非法 ID 直接拒绝
        if (id == null || id <= 0) return null;

        // ② 布隆过滤器：一定不存在的直接返回
        if (!productIdFilter.mightContain(id)) {
            metrics.recordPenetrationBlock();
            return null;   // 不查缓存、不查库
        }

        // ③ 缓存
        String cacheKey = "product:" + id;
        String cached = redis.opsForValue().get(cacheKey);
        if (cached != null) {
            if ("NULL_MARKER".equals(cached)) return null; // 空值标记
            return JSON.parseObject(cached, Product.class);
        }

        // ④ 查数据库
        Product product = productMapper.selectById(id);
        if (product == null) {
            // ⑤ 空值缓存：不存在的 key 也缓存，5 分钟
            redis.opsForValue().set(cacheKey, "NULL_MARKER", 5, TimeUnit.MINUTES);
        } else {
            redis.opsForValue().set(cacheKey, JSON.toJSONString(product),
                5 + ThreadLocalRandom.current().nextInt(60), TimeUnit.MINUTES);
        }
        return product;
    }
}
```

### 7.2 缓存击穿防护

**问题**: 热点 key 过期瞬间，大量请求同时打到底层。

```java
// === 方案：互斥锁 + 逻辑过期 + 热点预热 ===

@Service
public class CacheBreakdownGuard {

    public Product getProduct(Long id) {
        String cacheKey = "product:" + id;
        String lockKey = "lock:product:" + id;

        String cached = redis.opsForValue().get(cacheKey);
        if (cached != null) return JSON.parseObject(cached, Product.class);

        // 互斥锁：只允许一个人重建缓存
        Boolean locked = redis.opsForValue()
            .setIfAbsent(lockKey, "1", 10, TimeUnit.SECONDS);

        if (Boolean.TRUE.equals(locked)) {
            try {
                // 双重检查（拿到锁后再查一次）
                cached = redis.opsForValue().get(cacheKey);
                if (cached != null) return JSON.parseObject(cached, Product.class);

                // 真正重建
                Product product = productMapper.selectById(id);
                if (product != null) {
                    redis.opsForValue().set(cacheKey, JSON.toJSONString(product),
                        5, TimeUnit.MINUTES);
                }
                return product;
            } finally {
                redis.delete(lockKey);   // 释放锁
            }
        } else {
            // 没抢到锁：等一会儿，再查缓存
            Thread.sleep(50);
            return getProduct(id);
        }
    }
}
```

### 7.3 缓存雪崩防护

```java
// === 方案：TTL 随机抖动 + 多级缓存 + 限流兜底 ===

public void cacheProduct(Product product) {
    String key = "product:" + product.getId();

    // 过期时间加随机抖动，避免批量同时过期
    int baseTTL = 300;    // 基础 5 分钟
    int jitter = ThreadLocalRandom.current().nextInt(60);  // 随机 0-60 秒
    int finalTTL = baseTTL + jitter;

    redis.opsForValue().set(key, JSON.toJSONString(product),
        finalTTL, TimeUnit.SECONDS);

    // L1 本地缓存设置为更短 TTL
    localCache.put(product.getId(), product);
}
```

### 7.4 缓存一致性

```java
// === 方案：Cache Aside + 延迟双删 ===

@Transactional
public void updateProduct(Product product) {
    String cacheKey = "product:" + product.getId();

    // ① 先删缓存
    redis.delete(cacheKey);
    localCache.invalidate(product.getId());

    // ② 更新数据库
    productMapper.updateById(product);

    // ③ 延迟双删（异步，等主从同步完成后再次删除）
    CompletableFuture.runAsync(() -> {
        try {
            Thread.sleep(500);  // 等待主从同步（大于从库延迟）
            redis.delete(cacheKey);
            localCache.invalidate(product.getId());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }, delayDeleteExecutor);
}
```

### 7.5 分布式锁（防止超卖）

```java
// 扣减库存 — 防止并发超卖
@Transactional
public boolean deductStock(Long productId, int quantity) {
    // 乐观锁：WHERE stock >= ? 兜底
    int rows = productMapper.deductStock(productId, quantity);
    if (rows == 0) {
        return false;  // 库存不足
    }

    // 删缓存（缓存中的 stock 已过期）
    redis.delete("product:" + productId);
    localCache.invalidate(productId);
    return true;
}
```

```xml
<!-- MyBatis -->
<update id="deductStock">
    UPDATE products
    SET stock = stock - #{quantity}
    WHERE id = #{productId} AND stock >= #{quantity}
    <!--             ↑ 乐观锁，防止扣成负数           -->
</update>
```

---

## 8. 多环境配置管理

### 8.1 配置分离

```
src/main/resources/
├── application.yml              # 公共配置
├── application-dev.yml          # 开发环境 (本地)
├── application-test.yml         # 测试环境
├── application-prod.yml         # 生产环境
└── bootstrap.yml                # 配置中心地址
```

### 8.2 各环境差异

| 配置项 | dev | test | prod |
|--------|-----|------|------|
| 数据库连接池 | 5 | 10 | 20 |
| Redis 超时 | 5s | 3s | 2s |
| 慢查询阈值 | 1s | 500ms | 100ms |
| 日志级别 | DEBUG | INFO | WARN |
| 缓存 TTL | 10分钟 | 5分钟 | 1-5分钟+随机 |
| 限流 QPS | 不限制 | 1000 | 10000 |
| 熔断开关 | 关闭 | 半开 | 全开 |

### 8.3 敏感信息管理

```yaml
# ❌ 密码不在配置文件中
# application-prod.yml 里没有明文密码

# ✅ 从环境变量 / 密钥服务获取
spring:
  datasource:
    master:
      password: ${DB_MASTER_PASSWORD}   # K8s Secret / Vault 注入
  redis:
    password: ${REDIS_PASSWORD}

# 原理同 PPT CH08 讲的：配置文件隔一层，真实密码在服务器上
```

---

## 9. 故障处理与降级策略

### 9.1 三层降级链路

```
正常:
Nginx 限流 → 应用 → Redis → MySQL → 返回

一级降级（Redis 不可用）:
Nginx 限流 → 应用 → [X] → MySQL → 返回

二级降级（MySQL 不可用）:
Nginx 限流 → 应用 → Caffeine 本地缓存 → 返回旧数据 → 告警

三级降级（应用崩溃）:
Nginx 返回 502 + 静态降级页面
```

### 9.2 关键阈值

| 指标 | 阈值 | 超过后动作 |
|------|------|-----------|
| Redis 连接超时 | 3 次/30秒 | 跳过 Redis，直接查 MySQL |
| MySQL 连接池满 | > 80% | 限流，拒绝请求 |
| 接口响应 P99 | > 500ms | 告警，检查慢查询 |
| 缓存命中率 | < 80% | 检查 TTL 配置、容量 |
| 数据库 CPU | > 80% | 检查慢查询、扩容/读写分离 |
| JVM 堆使用率 | > 85% | 准备重启，检查内存泄漏 |

---

## 10. 监控、告警与可观测性

### 10.1 指标采集（Spring Boot Actuator + Micrometer）

```yaml
management:
  endpoints:
    web:
      exposure:
        include: health,metrics,prometheus,info
  metrics:
    tags:
      application: production-grade-cache
    export:
      prometheus:
        enabled: true
```

### 10.2 需要监控的指标

| 层面 | 指标 | 含义 |
|------|------|------|
| **应用** | L1/L2 缓存命中率 | 太低说明缓存策略有问题 |
| **应用** | 接口响应 P50/P99 | 用户实际体感 |
| **应用** | 连接池活跃连接数 | 接近 max 要扩容 |
| **应用** | 连接池等待时间 | 连接不够用了 |
| **应用** | GC 暂停时间 | 影响响应 P99 |
| **Redis** | 命中率、内存使用 | 快满了要扩容 |
| **Redis** | 已连接客户端数 | 连接数异常 |
| **MySQL** | 慢查询数量 | 新增慢查询要优化 |
| **MySQL** | 连接数 | 接近上限要处理 |
| **MySQL** | 主从延迟 | 超过 1 秒要处理 |
| **Nginx** | 4xx/5xx 错误率 | 业务/服务异常 |
| **Nginx** | 请求 QPS | 流量基线 |

### 10.3 日志规范

```java
// 每条日志带 traceId，串联整条链路
@Slf4j
public class ProductService {

    public Product getProduct(Long id) {
        log.info("[traceId={}] cache.L1.check productId={}", traceId(), id);

        // 慢查询告警
        long start = System.currentTimeMillis();
        Product product = productMapper.selectById(id);
        long elapsed = System.currentTimeMillis() - start;

        if (elapsed > 100) {
            log.warn("[traceId={}] SLOW_SQL productId={} elapsed={}ms",
                traceId(), id, elapsed);
        }

        return product;
    }
}
```

---

## 11. 安全体系

| 威胁 | 防护 |
|------|------|
| SQL 注入 | MyBatis `#{param}` 预编译，不用 `${}` |
| 横向越权 | 校验订单所属用户，不是当前用户的订单不返回 |
| 参数篡改 | `@Valid` + 参数校验，非法输入直接拒绝 |
| 暴力刷接口 | Nginx `limit_req` + IP 黑名单 |
| 敏感数据泄露 | 密码不记日志、不返回给前端、传输加密 |
| CSRF | 关键操作（下单/退款）验证 Referer + Token |
| 重放攻击 | 接口幂等性 Token（下单前先拿 Token，用一次失效） |

### 幂等性设计

```java
// 防止用户重复提交订单
@PostMapping("/order")
public Result<Order> createOrder(@RequestHeader String idempotentToken,
                                  @RequestBody OrderRequest req) {
    // ① 检查 token 是否已使用
    Boolean used = redis.setIfAbsent("idempotent:" + idempotentToken,
        "1", 5, TimeUnit.MINUTES);
    if (!used) {
        return Result.fail("请勿重复提交");  // 第二次提交被拦截
    }

    // ② 正常下单
    Order order = orderService.create(req);
    return Result.ok(order);
}
```

---

## 12. 部署方案

```
生产环境最少 5 台服务器:

┌─────────────────┐
│ Nginx × 1       │  公网入口，反向代理，限流
│ 公网 IP + 内网    │
└────────┬────────┘
         │
┌────────▼────────┐
│ Java 应用 × 2   │  应用集群，无状态，可水平扩展
│ 10.0.1.11:9000  │
│ 10.0.1.12:9000  │
└──┬──────────┬───┘
   │          │
┌──▼──────┐ ┌─▼──────────┐
│ Redis   │ │ MySQL 主库   │
│ Sentinel│ │ 10.0.2.20   │
│ × 3 节点 │ │ + 从库 × 1   │
│ 10.0.2.x│ │ 10.0.2.21   │
└─────────┘ └─────────────┘
```

### 部署检查清单

- [ ] 所有服务绑定内网 IP，不监听 0.0.0.0
- [ ] 安全组规则逐条审核（最小范围原则）
- [ ] MySQL `app_user` 只有 CRUD 没有 DDL
- [ ] 慢查询日志已开启
- [ ] Redis 哨兵已配置，自动故障转移已测试
- [ ] 配置文件不包含明文密码
- [ ] 备份脚本已部署并加入 crontab
- [ ] 健康检查接口已配置
- [ ] 监控面板已接入
- [ ] 告警规则已配置（Redis 宕机 / MySQL 慢查询 / 应用 5xx）

---

## 13. 启动检查清单

### 上线前必须确认

| 检查项 | 命令/方式 |
|--------|----------|
| 应用 ↔ MySQL 连通 | `telnet 10.0.2.20 3306` |
| 应用 ↔ Redis 连通 | `redis-cli -h 10.0.2.10 ping` |
| 主从同步正常 | `SHOW SLAVE STATUS\G` → `Seconds_Behind_Master: 0` |
| 慢查询日志开启 | `SHOW VARIABLES LIKE 'slow_query_log'` → ON |
| 从库可读 | 用 `read_user` 账号执行 `SELECT 1` |
| Nginx 配置正确 | `nginx -t` 无报错 |
| backup 账号可备份 | `mysqldump -u backup -p --all-databases --no-data` 正常 |
| 缓存预热完成 | 启动后批量加载热点商品到 Redis |
| 健康检查通过 | `curl http://10.0.1.11:9000/actuator/health` → UP |

---

> **这个文档里的每一项，都对应 PPT 里讲过的一个知识点，并且补充了 PPT 没讲的真实生产问题。**
> 
> 下一步：按这个设计逐一实现代码。
