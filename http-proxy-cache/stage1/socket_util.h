/*
 * socket_util.h — Socket工具模块
 *
 * 封装socket的创建、监听、连接等底层操作
 */

#pragma once

#include <string>

// 创建监听socket，返回文件描述符
int create_listen_socket(int port);

// 连接目标服务器，成功返回fd，失败返回-1
int connect_to_target(const std::string& host, int port);
