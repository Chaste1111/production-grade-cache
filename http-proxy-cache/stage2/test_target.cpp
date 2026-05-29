/*
 * test_target.cpp — 测试用的目标HTTP服务器
 *
 * 作用：模拟一个"数据服务器"，跑在9999端口。
 * 收到任何HTTP请求都返回一段固定JSON，方便验证代理是否正常工作。
 *
 * 编译：g++ -std=c++17 test_target.cpp -o test_target
 * 运行：./test_target
 * 测试：curl http://localhost:9999/
 */

#include <iostream>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    // ---- 第一步：创建socket ----
    // AF_INET = IPv4, SOCK_STREAM = TCP
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket 创建失败");
        return 1;
    }

    // 允许端口复用，防止重启时 "Address already in use"
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // ---- 第二步：绑定地址和端口 ----
    sockaddr_in addr{};
    addr.sin_family = AF_INET;          // IPv4
    addr.sin_port = htons(9999);        // 端口 9999，htons 把主机字节序转网络字节序
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡（0.0.0.0）

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind 失败");
        return 1;
    }

    // ---- 第三步：开始监听 ----
    // 参数5 = 内核维护的"等待连接队列"最大长度
    listen(listen_fd, 5);
    std::cout << "[目标服务器] 运行在 http://0.0.0.0:9999/" << std::endl;

    // 构造一个简单的HTTP响应体
    const char* body = R"({"status":"ok","from":"target_server","data":[1,2,3,4,5]})";
    int body_len = strlen(body);

    while (true) {
        // ---- 第四步：接受客户端连接 ----
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            perror("accept 失败");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "[目标服务器] 收到连接: " << client_ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // ---- 第五步：读取请求（不管内容，全读进来看看） ----
        char buf[4096];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << "[目标服务器] 收到请求:\n" << buf << std::endl;
        }

        // ---- 第六步：构造HTTP响应并发送 ----
        // HTTP响应格式：状态行 → 头部 → 空行 → 正文
        char response[8192];
        int resp_len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"           // 状态行
            "Content-Type: application/json\r\n"  // 内容类型
            "Content-Length: %d\r\n"        // 正文长度
            "Connection: close\r\n"         // 告诉客户端用完就断开
            "X-Server: test-target\r\n"     // 自定义头，方便识别
            "\r\n"                          // 空行，分隔头部和正文
            "%s",                           // 正文
            body_len, body);

        write(client_fd, response, resp_len);
        std::cout << "[目标服务器] 已发送响应 (" << resp_len << " 字节)" << std::endl;

        // ---- 第七步：关闭连接 ----
        close(client_fd);
        std::cout << "[目标服务器] 连接关闭" << std::endl;
    }

    close(listen_fd);
    return 0;
}
