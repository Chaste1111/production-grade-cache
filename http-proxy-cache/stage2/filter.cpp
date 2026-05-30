/*
 * filter.cpp — 黑白名单过滤实现
 */

#include "filter.h"
using namespace std;

void Filter::add_black(const string& host) {
    blacklist_.push_back(host);
}

void Filter::add_white(const string& host) {
    whitelist_.push_back(host);
}

bool Filter::is_allowed(const string& host) const {
    // 1. 先查黑名单，命中就拒绝
    if (match_any(host, blacklist_))
        return false;

    // 2. 如果启用了白名单（非空），不在白名单就拒绝
    if (!whitelist_.empty() && !match_any(host, whitelist_))
        return false;

    return true;
}

bool Filter::match_any(const string& host, const vector<string>& rules) const {
    for (const auto& rule : rules) {
        if (host.find(rule) != string::npos)
            return true;  // 子串匹配
    }
    return false;
}
