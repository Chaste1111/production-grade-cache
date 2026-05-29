/*
 * proxy_handler.h — 代理核心逻辑模块
 *
 * 处理一次完整的代理请求：读 → 解析 → 转发 → 回传
 */

#pragma once

// 处理浏览器发来的一次代理请求
// browser_fd: 浏览器连接的socket
void handle_proxy_request(int browser_fd);
