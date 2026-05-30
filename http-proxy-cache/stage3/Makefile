# Makefile — Stage3: epoll 非阻塞 + 状态机

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

PROXY_OBJS = proxy_server.o connection.o epoller.o lru_cache.o \
             filter.o file_logger.o header_mod.o \
             http_parser.o url_parser.o socket_util.o

all: proxy_server

proxy_server: $(PROXY_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

proxy_server.o: proxy_server.cpp socket_util.h epoller.h connection.h \
                lru_cache.h filter.h file_logger.h cache_interface.h
connection.o: connection.cpp connection.h epoller.h common.h http_parser.h \
              url_parser.h socket_util.h cache_interface.h filter.h logger_interface.h
epoller.o: epoller.cpp epoller.h
lru_cache.o: lru_cache.cpp lru_cache.h cache_interface.h
filter.o: filter.cpp filter.h
file_logger.o: file_logger.cpp file_logger.h logger_interface.h
header_mod.o: header_mod.cpp header_mod.h
http_parser.o: http_parser.cpp http_parser.h common.h
url_parser.o: url_parser.cpp url_parser.h common.h
socket_util.o: socket_util.cpp socket_util.h

clean:
	rm -f proxy_server *.o proxy_access.log

run: proxy_server
	./proxy_server

.PHONY: all clean run
