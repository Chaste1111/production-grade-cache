/*
 * filter.h — 黑白名单过滤模块
 *
 * 黑名单优先：在黑名单中的主机直接拒绝
 * 白名单：启用后只允许白名单中的主机，不在白名单的拒绝
 */

#pragma once

#include <string>
#include <vector>

class Filter {
public:
    // 黑名单：这些主机/网段禁止访问
    void add_black(const std::string& host);
    // 白名单：只允许这些主机/网段（为空则不启用白名单）
    void add_white(const std::string& host);

    // 检查是否允许访问，返回 true 表示允许
    bool is_allowed(const std::string& host) const;

private:
    // 匹配规则：host 中是否包含 pattern（子串匹配）
    bool match_any(const std::string& host, const std::vector<std::string>& rules) const;

    std::vector<std::string> blacklist_;
    std::vector<std::string> whitelist_;
};
