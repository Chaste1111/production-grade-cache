/*
 * proxy_handler.h — 代理核心逻辑模块（第二阶段：带缓存）
 */

#pragma once

#include "cache_interface.h"
#include "filter.h"
#include "logger_interface.h"

// 处理浏览器发来的一次代理请求
void handle_proxy_request(int browser_fd);

// 注入缓存实现（在 main 中调用，实现与接口分离）
void set_cache(CacheInterface* cache);
void set_filter(Filter* filter);
void set_logger(LoggerInterface* logger);

// 缓存统计
int cache_hit_count();
int cache_miss_count();
int cache_hit_rate();
