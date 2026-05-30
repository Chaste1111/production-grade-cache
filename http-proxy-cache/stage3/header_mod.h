/*
 * header_mod.h — 请求头修改模块
 *
 * 在代理转发请求前，添加/删除/修改 HTTP 头部
 */

#pragma once

#include <string>
#include <vector>

struct HeaderRule {
    std::string action;  // "add", "remove", "replace"
    std::string name;    // 头部名称，如 "User-Agent"
    std::string value;   // add/replace 时的值，remove 时忽略
};

class HeaderMod {
public:
    // 添加一条修改规则
    void add_rule(const std::string& action,
                  const std::string& name,
                  const std::string& value = "");

    // 对原始 HTTP 请求头应用所有规则，返回修改后的头部
    std::string apply(const std::string& raw_headers) const;

private:
    std::vector<HeaderRule> rules_;
};
