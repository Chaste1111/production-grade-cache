# Makefile — 第二阶段：带LRU缓存的代理

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

# 比第一阶段多了一个 lru_cache.o
PROXY_OBJS = proxy_server.o proxy_handler.o lru_cache.o \
             http_parser.o url_parser.o socket_util.o

all: proxy_server test_target

proxy_server: $(PROXY_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

proxy_server.o: proxy_server.cpp socket_util.h proxy_handler.h
proxy_handler.o: proxy_handler.cpp proxy_handler.h common.h http_parser.h url_parser.h socket_util.h lru_cache.h
lru_cache.o: lru_cache.cpp lru_cache.h
http_parser.o: http_parser.cpp http_parser.h common.h
url_parser.o: url_parser.cpp url_parser.h common.h
socket_util.o: socket_util.cpp socket_util.h

test_target: test_target.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f proxy_server test_target *.o

run: proxy_server
	./proxy_server

.PHONY: all clean run
