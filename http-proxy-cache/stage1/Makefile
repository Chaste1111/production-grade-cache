# Makefile — 第一阶段：单线程阻塞代理
#
# 目标：
#   make          编译所有
#   make clean    清理编译产物
#   make run      编译并启动代理
#   make test     启动测试目标服务器

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

# 代理服务器需要的 .o 文件
PROXY_OBJS = proxy_server.o proxy_handler.o http_parser.o url_parser.o socket_util.o

all: proxy_server test_target

# ---- 代理服务器 ----
proxy_server: $(PROXY_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

proxy_server.o: proxy_server.cpp socket_util.h proxy_handler.h
proxy_handler.o: proxy_handler.cpp proxy_handler.h common.h http_parser.h url_parser.h socket_util.h
http_parser.o: http_parser.cpp http_parser.h common.h
url_parser.o: url_parser.cpp url_parser.h common.h
socket_util.o: socket_util.cpp socket_util.h

# ---- 测试目标服务器（独立编译，不依赖代理模块） ----
test_target: test_target.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

# ---- 工具命令 ----
clean:
	rm -f proxy_server test_target *.o

run: proxy_server
	./proxy_server

runtest: test_target
	./test_target

.PHONY: all clean run runtest
