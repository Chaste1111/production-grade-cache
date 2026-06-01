# production-grade-cache

> 生产级缓存架构 — Java 应用服务器实战项目

电商商品详情页场景，QPS 10,000，读多写少。从 Nginx 入口到 MySQL 落盘，四层全链路实现。覆盖 CH07（Web服务器与Nginx）和 CH08（数据库和缓存）的全部知识点。

**详细架构设计**：[ARCHITECTURE.md](./ARCHITECTURE.md)

---

## 目录

- [技术栈](#技术栈)
- [环境要求](#环境要求)
- [开发环境搭建](#开发环境搭建)
- [项目结构](#项目结构)
- [快速启动](#快速启动)
- [测试](#测试)
- [部署](#部署)

---

## 技术栈

| 层 | 技术 | 版本 |
|----|------|------|
| 接入层 | Nginx | 1.26+ |
| 应用层 | Spring Boot 3.x | 内嵌 Tomcat |
| 本地缓存 | Caffeine | 3.x |
| 分布式缓存 | Redis | 7.x+ |
| 数据库 | MySQL | 8.0+ |
| 连接池 | HikariCP | 5.x（Spring Boot 默认） |
| JDK | Java 17 | LTS |
| 构建 | Maven | 3.9+ |

---

## 环境要求

| 软件 | 用途 | 最低版本 |
|------|------|----------|
| Java JDK | 编译运行 Java 代码 | 17 LTS |
| Maven | 项目构建、依赖管理 | 3.9 |
| MySQL | 持久化存储 | 8.0 |
| Redis | 分布式缓存 | 7.0 |
| Nginx | 反向代理、限流、负载均衡 | 1.26 |

---

## 开发环境搭建

### 方案 A：apt + SDKMAN（推荐，有 sudo）

适合：公司配的开发机、云服务器、自己装的 Linux。

#### 第一步：系统服务（apt 一把梭）

```bash
# 更新源
sudo apt update

# 安装 MySQL、Redis、Nginx
sudo apt install -y mysql-server redis-server nginx

# 启动并设置开机自启
sudo systemctl enable mysql redis-server nginx --now

# 验证
mysql --version
redis-cli ping
nginx -v
```

#### 第二步：Java 生态（SDKMAN 管版本）

SDKMAN 是 Java 圈标准的多版本管理工具，一个命令安装/切换 Java 和 Maven，不需要自己去官网下载。

```bash
# 1. 安装 SDKMAN
curl -s "https://get.sdkman.io" | bash
source "$HOME/.sdkman/bin/sdkman-init.sh"

# 2. 安装 Java 17（LTS 长期支持版本）
sdk install java 17.0.19-tem

# 3. 安装 Maven
sdk install maven

# 4. 设为默认版本
sdk default java 17.0.19-tem
sdk default maven 3.9.16

# 5. 验证
java -version   # OpenJDK 17.0.19
mvn -version    # Apache Maven 3.9.16
```

> **为什么用 SDKMAN 而不是 apt 装 Java？**
> `apt install openjdk-17-jdk` 只能装一个版本。将来要切 Java 21 或同时维护多版本，SDKMAN 一个命令就够了。Java 社区普遍使用 SDKMAN。

#### 第三步：MySQL 初始化

```bash
# 登录（初次安装通常用 sudo mysql）
sudo mysql

# 创建数据库
CREATE DATABASE shop DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

# 创建业务账号（PPT CH08：账号权限分离）
CREATE USER 'app_user'@'localhost' IDENTIFIED BY 'your_app_password';
GRANT SELECT, INSERT, UPDATE, DELETE ON shop.* TO 'app_user'@'localhost';

# 创建只读账号（数据分析、报表）
CREATE USER 'read_user'@'localhost' IDENTIFIED BY 'your_read_password';
GRANT SELECT ON shop.* TO 'read_user'@'localhost';

FLUSH PRIVILEGES;
EXIT;
```

#### 第四步：Redis 配置

```bash
# 编辑 /etc/redis/redis.conf
sudo vim /etc/redis/redis.conf

# 关键配置：
bind 127.0.0.1                    # 只监听本机（开发环境）
maxmemory 256mb                   # 内存上限
maxmemory-policy allkeys-lru      # 淘汰策略：LRU

sudo systemctl restart redis-server
```

#### 第五步：配置环境变量

SDKMAN 和 apt 会自动配好 `PATH`，通常不需要手动加。

如需确认，打开新终端后执行：

```bash
echo $JAVA_HOME     # /home/你的用户名/.sdkman/candidates/java/current
echo $MAVEN_HOME    # /home/你的用户名/.sdkman/candidates/maven/current
which java          # 应该是 sdkman 路径
```

---

### 方案 B：Docker（更省事）

适合：所有环境一致、不想污染本机。

```bash
# 安装 Docker
sudo apt install docker.io docker-compose-v2

# 启动所有依赖
docker run -d --name mysql -p 3306:3306 -e MYSQL_ROOT_PASSWORD=root mysql:8.0
docker run -d --name redis -p 6379:6379 redis:7
docker run -d --name nginx -p 80:80 nginx

# Java 和 Maven 仍用 SDKMAN
```

---

### 方案 C：手动安装（无 sudo、离线环境）

参考本项目实际开发过程的记录。核心步骤：

```bash
# 1. Java JDK
# 从 Adoptium 下载二进制包，解压到 ~/.local/java17/
# https://adoptium.net/download/

# 2. Maven
# 从 Apache 下载二进制包，解压到 ~/.local/maven/
# https://maven.apache.org/download.cgi

# 3. MySQL
# 用系统已有的或请求运维安装

# 4. Redis
# 从 redis.io 下载源码编译
# 或从 GitHub Release 下载二进制
```

```bash
# 手动添加 PATH
cat >> ~/.bashrc << 'EOF'
export JAVA_HOME="$HOME/.local/java17"
export MAVEN_HOME="$HOME/.local/maven"
export PATH="$JAVA_HOME/bin:$MAVEN_HOME/bin:$HOME/.local/bin:$PATH"
EOF
source ~/.bashrc
```

---

### 环境检查清单

安装完成后逐条验证：

```bash
# 1. Java
java -version        # openjdk version "17.0.19"

# 2. Maven
mvn -version         # Apache Maven 3.9.16

# 3. MySQL
mysql -u app_user -p -e "SELECT 1"   # 能连上
systemctl status mysql               # active (running)

# 4. Redis
redis-cli ping       # PONG
systemctl status redis-server        # active (running)

# 5. Nginx
nginx -t             # syntax is ok
nginx -v             # nginx/1.26.x

# 6. 端口确认
ss -tlnp | grep 3306   # MySQL
ss -tlnp | grep 6379   # Redis
ss -tlnp | grep 80     # Nginx
```

---

## 项目结构

```
production-grade-cache/
├── README.md                   ← 你在这里
├── ARCHITECTURE.md             ← 架构设计文档（13章）
│
├── pom.xml                     ← Maven 依赖配置
├── src/
│   ├── main/
│   │   ├── java/com/shop/
│   │   │   ├── ShopApplication.java        ← 启动入口
│   │   │   │
│   │   │   ├── controller/                 ← 控制器（接收HTTP请求）
│   │   │   │   └── ProductController.java
│   │   │   │
│   │   │   ├── service/                    ← 业务逻辑层
│   │   │   │   └── ProductService.java     ← 多级缓存 + 业务逻辑
│   │   │   │
│   │   │   ├── repository/                 ← 数据访问层
│   │   │   │   └── ProductMapper.java
│   │   │   │
│   │   │   ├── config/                     ← 配置类
│   │   │   │   ├── CacheConfig.java        ← Caffeine + Redis 配置
│   │   │   │   ├── DataSourceConfig.java   ← 读写分离数据源
│   │   │   │   └── RedisConfig.java        ← Redis 连接池
│   │   │   │
│   │   │   ├── cache/                      ← 缓存模块
│   │   │   │   ├── CachePenetration.java   ← 穿透防护
│   │   │   │   ├── CacheBreakdown.java     ← 击穿防护
│   │   │   │   └── CacheConsistency.java   ← 一致性策略
│   │   │   │
│   │   │   └── model/                      ← 数据模型
│   │   │       └── Product.java
│   │   │
│   │   └── resources/
│   │       ├── application.yml             ← 公共配置
│   │       ├── application-dev.yml         ← 开发环境
│   │       ├── application-prod.yml        ← 生产环境
│   │       └── schema.sql                  ← 数据库建表语句
│   │
│   └── test/
│       └── java/com/shop/
│           └── ProductServiceTest.java
│
└── nginx/
    └── nginx.conf              ← Nginx 配置文件（参考用）
```

---

## 快速启动

```bash
# 1. 克隆仓库
git clone https://github.com/Chaste1111/production-grade-cache.git
cd production-grade-cache

# 2. 确认环境
java -version && mvn -version && redis-cli ping && mysql -u app_user -p -e "SELECT 1"

# 3. 编译
mvn clean package -DskipTests

# 4. 启动
java -jar target/shop-0.0.1.jar --spring.profiles.active=dev

# 5. 测试接口
curl http://localhost:9000/product/1
```

---

## 测试

```bash
# 单元测试
mvn test

# 指定测试类
mvn test -Dtest=ProductServiceTest

# 性能测试（需要 ab 或 wrk）
ab -n 10000 -c 100 http://localhost:9000/product/1
```

---

## 部署

### 开发环境

```bash
java -jar target/shop-0.0.1.jar --spring.profiles.active=dev
```

- 连接池 5
- 慢查询阈值 1s
- 日志级别 DEBUG
- 缓存 TTL 10 分钟

### 生产环境

```bash
java -jar target/shop-0.0.1.jar --spring.profiles.active=prod
```

- 连接池 20
- 慢查询阈值 100ms
- 日志级别 WARN
- 缓存 TTL 5 分钟 + 随机抖动

---

## 相关仓库

| 仓库 | 关联 |
|------|------|
| [nginx-source-code-analysis](https://github.com/Chaste1111/nginx-source-code-analysis) | C++ HTTP 代理缓存服务器，本项目的上游入口 |
| [-claude-mysql-](https://github.com/Chaste1111/-claude-mysql-) | MySQL 学习笔记 |

---

## 参考资料

- [Spring Boot 官方文档](https://docs.spring.io/spring-boot/docs/current/reference/htmlsingle/)
- [MyBatis-Plus 文档](https://baomidou.com/)
- [Redis 官方文档](https://redis.io/docs/latest/)
- [MySQL 8.0 参考手册](https://dev.mysql.com/doc/refman/8.0/en/)
- [HikariCP 配置指南](https://github.com/brettwooldridge/HikariCP)
- [Caffeine 缓存文档](https://github.com/ben-manes/caffeine/wiki)
- [课程 PPT：CH07 Web服务器与Nginx](../学习笔记/铁哥/CH07%20Web服务器与Nginx.pdf)
- [课程 PPT：CH08 数据库和缓存](../学习笔记/铁哥/CH08%20数据库和缓存.pdf)
