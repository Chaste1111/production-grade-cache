/*
 * proxy_handler.h — 代理核心逻辑模块（第二阶段：带缓存）
 */

#pragma once

// 处理浏览器发来的一次代理请求
void handle_proxy_request(int browser_fd);
