/*
 * header_mod.cpp — 请求头修改实现
 */

#include "header_mod.h"
#include <sstream>
using namespace std;

void HeaderMod::add_rule(const string& action, const string& name, const string& value) {
    rules_.push_back({action, name, value});
}

string HeaderMod::apply(const string& raw_headers) const {
    if (rules_.empty()) return raw_headers;

    istringstream input(raw_headers);
    ostringstream output;
    string line;

    // 处理请求行（第一行），原样输出
    if (getline(input, line) && !line.empty() && line.back() == '\r')
        line.pop_back();
    output << line << "\r\n";

    // 处理头部行
    while (getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // 空行 = 头部结束

        // 检查是否匹配任何 remove 或 replace 规则
        bool skip = false;
        string replace_value;

        for (const auto& r : rules_) {
            // 头部格式: "Name: value"
            size_t colon = line.find(':');
            if (colon == string::npos) continue;
            string header_name = line.substr(0, colon);

            if (header_name == r.name) {
                if (r.action == "remove") {
                    skip = true;
                    break;
                }
                if (r.action == "replace") {
                    replace_value = r.name + ": " + r.value;
                    break;
                }
            }
        }

        if (!skip) {
            if (!replace_value.empty())
                output << replace_value << "\r\n";
            else
                output << line << "\r\n";
        }
    }

    // 添加 add 规则的头部
    for (const auto& r : rules_) {
        if (r.action == "add")
            output << r.name << ": " << r.value << "\r\n";
    }

    output << "\r\n";  // 头部结束
    return output.str();
}
