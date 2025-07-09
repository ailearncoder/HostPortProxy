/**
 * @file http_reverse_proxy.cpp
 * @brief 一个轻量级的HTTP反向代理服务器。
 *
 * 该代理服务器监听一个固定端口（默认为8080），并根据传入HTTP请求的Host头动态地将请求转发到本地（127.0.0.1）的不同端口。
 * 例如，如果Host头是 "12345.example.com"，请求将被转发到 127.0.0.1:12345。
 *
 * 主要特性：
 * - 动态端口转发：基于Host头解析目标端口。
 * - 高性能：使用epoll进行I/O多路复用，支持大量并发连接。
 * - 非阻塞I/O：所有网络操作均为非阻塞。
 * - 错误处理：包含基本的HTTP错误响应。
 * - 日志记录：提供调试和信息日志。
 *
 * 用途：
 * - 本地开发环境中的多服务统一入口。
 * - 灵活的端口映射和测试。
 *
 * @author AilearnCode
 * @date 2025-07-09
 * @version 1.0
 */

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <algorithm>

#define LISTEN_PORT 8080
#define MAX_EVENTS 1024
#define BUFFER_SIZE 16384
#define HEADER_END_MARKER "\r\n\r\n"

#define LOG_DEBUG(args, ...) printf("%s DEBUG: " args "\n", get_current_time().c_str(), ##__VA_ARGS__)
#define LOG_INFO(args, ...) printf("%s INFO: " args "\n",  get_current_time().c_str(), ##__VA_ARGS__)
#define LOG_ERROR(args, ...) printf("%s ERROR: " args "\n", get_current_time().c_str(), ##__VA_ARGS__)

#include <csignal>
#include <list>

volatile sig_atomic_t running = 1;

void handle_sigint(int sig) {
    running = 0;
}

enum connection_stage {
    READING_HEADER,
    CONNECTING,
    FORWARDING
};

struct connection {
    int client_fd = -1;
    int target_fd = -1;
    connection_stage stage = READING_HEADER;
    
    std::vector<char> client_buffer;
    std::vector<char> target_buffer;
    
    std::string host_port;
    int target_port = 0;
    
    bool client_eof = false;
    bool target_eof = false;
};

std::string get_current_time() {
    // 2025-01-01 23:59:59.000
    char buffer[64] = {0};
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", t);
    // 添加毫秒
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    long long ms = tv.tv_usec / 1000;
    snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), ".%03lld", ms);
    return std::string(buffer);
}

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static std::list<connection*> active_connections;

void close_connection(int epoll_fd, connection* conn) {
    if (conn->client_fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, nullptr);
        LOG_DEBUG("Closing client connection: %d", conn->client_fd);
        close(conn->client_fd);
    }
    if (conn->target_fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->target_fd, nullptr);
        LOG_DEBUG("Closing target connection: %d", conn->target_fd);
        close(conn->target_fd);
    }
    active_connections.remove(conn);
    delete conn;
}

void handle_client_data(int epoll_fd, connection* conn);
void handle_target_data(int epoll_fd, connection* conn);

void forward_to_target(int epoll_fd, connection* conn) {
    // 创建目标socket
    conn->target_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->target_fd < 0) {
        const char response[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        send(conn->client_fd, response, sizeof(response)-1, 0);
        close_connection(epoll_fd, conn);
        return;
    }
    
    set_nonblock(conn->target_fd);
    
    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(conn->target_port);
    target_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // 非阻塞连接
    int ret = connect(conn->target_fd, (sockaddr*)&target_addr, sizeof(target_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close_connection(epoll_fd, conn);
        return;
    }
    
    // 注册目标socket到epoll
    epoll_event ev{};
    ev.events = EPOLLOUT | EPOLLET;
    ev.data.ptr = conn;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->target_fd, &ev);
    
    conn->stage = CONNECTING;
}

void handle_read_header(connection* conn, ssize_t bytes_read) {
    // 追加数据到缓冲区
    char* buf = conn->client_buffer.data() + conn->client_buffer.size();
    conn->client_buffer.resize(conn->client_buffer.size() + bytes_read);
    
    // 查找请求头结束标记
    auto end_pos = std::search(
        conn->client_buffer.begin(), conn->client_buffer.end(),
        HEADER_END_MARKER, HEADER_END_MARKER + 4
    );
    
    if (end_pos == conn->client_buffer.end()) {
        if (conn->client_buffer.size() > BUFFER_SIZE) {
            const char response[] = "HTTP/1.1 413 Request Entity Too Large\r\n\r\n";
            send(conn->client_fd, response, sizeof(response)-1, 0);
            throw std::runtime_error("Header too large");
        }
        return;
    }
    
    // 解析Host头
    std::string header(conn->client_buffer.begin(), end_pos + 4);
    std::transform(header.begin(), header.end(), header.begin(), ::tolower);
    
    size_t host_start = header.find("host: ");
    if (host_start == std::string::npos) {
        const char response[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(conn->client_fd, response, sizeof(response)-1, 0);
        throw std::runtime_error("Missing Host header");
    }
    
    host_start += 6;
    size_t host_end = header.find("\r\n", host_start);
    if (host_end == std::string::npos) {
        const char response[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(conn->client_fd, response, sizeof(response)-1, 0);
        throw std::runtime_error("Invalid Host header");
    }
    
    std::string host = header.substr(host_start, host_end - host_start);
    conn->host_port = host;
    size_t dot_pos = host.find('.');
    if (dot_pos == std::string::npos || dot_pos == 0) {
        const char response[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(conn->client_fd, response, sizeof(response)-1, 0);
        throw std::runtime_error("Invalid Host format");
    }
    
    try {
        conn->target_port = std::stoi(host.substr(0, dot_pos));
    } catch (...) {
        const char response[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(conn->client_fd, response, sizeof(response)-1, 0);
        throw std::runtime_error("Invalid port number");
    }
    
    if (conn->target_port < 1 || conn->target_port > 65535) {
        const char response[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(conn->client_fd, response, sizeof(response)-1, 0);
        throw std::runtime_error("Port out of range");
    }
}

void handle_connect_result(int epoll_fd, connection* conn) {
    // 检查连接结果
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(conn->target_fd, SOL_SOCKET, SO_ERROR, &error, &len);
    
    if (error != 0) {
        LOG_ERROR("%s Failed to connect target port %d: %s", conn->host_port.c_str(), conn->target_port, strerror(error));
        std::ostringstream content;
        content << "{\"code\": 502, \"message\": \"Failed to connect target port " << conn->target_port << "\"}";
        std::ostringstream response;
        response << "HTTP/1.1 502 Bad Gateway\r\n";
        response << "Connection: close\r\n";
        response << "Content-Type: Application/json\r\n";
        response << "Content-Length: " << content.str().size() << "\r\n";
        response << "\r\n";
        response << content.str();
        send(conn->client_fd, response.str().data(), response.str().size(), 0);
        close_connection(epoll_fd, conn);
        return;
    }
    
    // 修改epoll事件为读写监听
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = conn;
    LOG_DEBUG("Registering EPOLLOUT on target connection");
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->target_fd, &ev);
    
    // 发送已缓冲的请求数据
    if (!conn->client_buffer.empty()) {
        ssize_t sent = send(conn->target_fd, 
                          conn->client_buffer.data(), 
                          conn->client_buffer.size(), 
                          MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno != EAGAIN) {
                close_connection(epoll_fd, conn);
                return;
            }
        } else if (static_cast<size_t>(sent) < conn->client_buffer.size()) {
            // 保留未发送的数据
            conn->client_buffer.erase(
                conn->client_buffer.begin(),
                conn->client_buffer.begin() + sent
            );
        } else {
            conn->client_buffer.clear();
        }
    }
    
    // 注册客户端socket的读事件
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = conn;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->client_fd, &ev);
    
    conn->stage = FORWARDING;
}

int main() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblock(listen_fd);
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return -1;
    }
    
    if (listen(listen_fd, SOMAXCONN) < 0) {
        close(listen_fd);
        return -1;
    }
    
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        close(listen_fd);
        return -1;
    }
    
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
    
    epoll_event events[MAX_EVENTS];
    
    while (running) {
        LOG_DEBUG("Entering epoll_wait");
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                if (!running) {
                    break;
                }
                continue;
            }
            LOG_ERROR("epoll_wait error: %s", strerror(errno));
            continue;
        }
        LOG_DEBUG("epoll_wait returned %d", n);
        
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == listen_fd) {
                // 处理新连接
                LOG_INFO("New connection");
                sockaddr_in client_addr{};
                socklen_t len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
                if (client_fd < 0) {
                    continue;
                }
                set_nonblock(client_fd);
                connection* conn = new connection();
                active_connections.push_back(conn);
                conn->client_fd = client_fd;
                conn->client_buffer.reserve(BUFFER_SIZE);
                
                epoll_event client_ev{};
                client_ev.events = EPOLLIN | EPOLLET;
                client_ev.data.ptr = conn;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
            } else {
                connection* conn = static_cast<connection*>(events[i].data.ptr);
                
                try {
                    if (events[i].events & EPOLLERR) {
                        LOG_DEBUG("EPOLLERR occurred: %X, error: %s (%d)", events[i].events, strerror(errno), errno);
                        if (errno != EINPROGRESS) {
                            std::ostringstream os;
                            os << "EPOLLERR occurred: " << strerror(errno) << " (" << errno << ")";
                            throw std::runtime_error(os.str());
                        }
                    }
                    
                    if (conn->stage == READING_HEADER) {
                        // 读取客户端请求头
                        LOG_DEBUG("READING_HEADER");
                        char buffer[BUFFER_SIZE];
                        ssize_t bytes_read = 0;
                        while ((bytes_read = recv(conn->client_fd, buffer, sizeof(buffer), 0)) > 0) {
                            conn->client_buffer.insert(conn->client_buffer.end(), buffer, buffer + bytes_read);
                            handle_read_header(conn, bytes_read);
                        }
                        
                        if (bytes_read < 0 && errno != EAGAIN) {
                            throw std::runtime_error("Read error");
                        }
                        
                        // 还没解析到端口号，继续读取
                        if (conn->target_port == 0) {
                            continue; // 头还没读完
                        }
                        
                        // 开始连接目标服务器
                        forward_to_target(epoll_fd, conn);
                    } 
                    else if (conn->stage == CONNECTING) {
                        // 处理连接结果
                        LOG_DEBUG("CONNECTING");
                        handle_connect_result(epoll_fd, conn);
                    }
                    else if (conn->stage == FORWARDING) {
                        // 数据转发处理
                        LOG_DEBUG("FORWARDING %X", events[i].events);
                        if (events[i].events & EPOLLIN) {
                            LOG_DEBUG("Reading data");
                            if (events[i].data.fd == conn->client_fd) {
                                LOG_DEBUG("Client data -> Target");
                                handle_client_data(epoll_fd, conn);
                            } else {
                                LOG_DEBUG("Target data -> Client");
                                handle_target_data(epoll_fd, conn);
                            }
                        }
                        if (events[i].events & EPOLLOUT) {
                            // 处理写缓冲区
                            LOG_DEBUG("Sending data");
                            if (!conn->client_buffer.empty()) {
                                LOG_DEBUG("Sending client data to target");
                                ssize_t sent = send(conn->target_fd, 
                                                  conn->client_buffer.data(),
                                                  conn->client_buffer.size(),
                                                  MSG_NOSIGNAL);
                                if (sent < 0) {
                                    if (errno != EAGAIN) {
                                        std::ostringstream os;
                                        os << "Send target error: " << strerror(errno) << " (" << errno << ")";
                                        throw std::runtime_error(os.str());
                                    }
                                } else {
                                    conn->client_buffer.erase(
                                        conn->client_buffer.begin(),
                                        conn->client_buffer.begin() + sent
                                    );
                                }
                            }
                            
                            if (!conn->target_buffer.empty()) {
                                LOG_DEBUG("Sending target data to client");
                                ssize_t sent = send(conn->client_fd,
                                                  conn->target_buffer.data(),
                                                  conn->target_buffer.size(),
                                                  MSG_NOSIGNAL);
                                if (sent < 0) {
                                    if (errno != EAGAIN) {
                                        std::ostringstream os;
                                        os << "Send client error: " << strerror(errno) << " (" << errno << ")";
                                        throw std::runtime_error(os.str());
                                    }
                                } else {
                                    LOG_DEBUG("Sent %zd bytes to client", sent);
                                    conn->target_buffer.erase(
                                        conn->target_buffer.begin(),
                                        conn->target_buffer.begin() + sent
                                    );
                                }
                            }
                            if (conn->target_eof && conn->target_buffer.empty()) {
                                LOG_DEBUG("Target EOF and buffer empty, closing connection");
                                close_connection(epoll_fd, conn);
                                conn = nullptr;
                            } else if (conn->client_eof && conn->client_buffer.empty()) {
                                LOG_DEBUG("Client EOF and buffer empty, closing connection");
                                close_connection(epoll_fd, conn);
                                conn = nullptr;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Caught exception: %s", e.what());
                    if (conn) {
                        close_connection(epoll_fd, conn);
                    }
                }
            }
        }
    }

    // 清理所有剩余连接
    for (auto conn : active_connections) {
        if (conn->client_fd != -1) {
            close(conn->client_fd);
        }
        if (conn->target_fd != -1) {
            close(conn->target_fd);
        }
        delete conn;
    }
    active_connections.clear();
    
    close(epoll_fd);
    close(listen_fd);
    LOG_INFO("Server stopped");
    return 0;
}

void handle_client_data(int epoll_fd, connection* conn) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = 0;
    ssize_t bytes_read_total = 0;
    while ((bytes_read = recv(conn->client_fd, buffer, sizeof(buffer), 0)) > 0) {
        // 转发到目标缓冲区
        conn->client_buffer.insert(conn->client_buffer.end(), buffer, buffer + bytes_read);
        bytes_read_total += bytes_read;
        ssize_t sent = send(conn->target_fd,
                    conn->client_buffer.data(),
                    conn->client_buffer.size(),
                    MSG_NOSIGNAL);
        if (sent > 0) {
            conn->client_buffer.erase(
                conn->client_buffer.begin(),
                conn->client_buffer.begin() + sent
            );
        }
        if (-1 == sent) {
            if (errno != EAGAIN) {
                std::ostringstream os;
                os << "Send target error: " << strerror(errno) << " (" << errno << ")";
                throw std::runtime_error(os.str());
            }
        }
        LOG_DEBUG("Received %zd bytes from client, sent %zd", bytes_read_total, sent);
    }

    LOG_DEBUG("Total bytes read from client: %zd,%zd, left: %zd", bytes_read_total, bytes_read, conn->client_buffer.size());
    
    if (bytes_read == 0) {
        conn->client_eof = true;
        LOG_DEBUG("Client EOF, closing target connection");
        // 这里关闭了链接，会导致target提前断开连接，后面缓冲区数据发送到target会失败，所以注释掉
        // shutdown(conn->target_fd, SHUT_WR);
    } else if (errno != EAGAIN) {
        throw std::runtime_error("Client read error");
    }
    
    // 尝试立即发送缓冲数据
    if (!conn->client_buffer.empty()) {
        ssize_t sent = send(conn->target_fd, 
                          conn->client_buffer.data(),
                          conn->client_buffer.size(), 
                          MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno != EAGAIN) {
                std::ostringstream os;
                os << "Send target error: " << strerror(errno) << " (" << errno << ")";
                throw std::runtime_error(os.str());
            }
        } else {
            conn->client_buffer.erase(
                conn->client_buffer.begin(),
                conn->client_buffer.begin() + sent
            );
        }
    }
    
    // 如果缓冲区仍有数据，注册写事件
    if (!conn->client_buffer.empty()) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = conn;
        LOG_DEBUG("Registering EPOLLOUT on target connection");
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->target_fd, &ev);
    }
}

void handle_target_data(int epoll_fd, connection* conn) {
    char buffer[BUFFER_SIZE] = {0};
    ssize_t bytes_read = 0;
    ssize_t bytes_read_total = 0;
    while ((bytes_read = recv(conn->target_fd, buffer, sizeof(buffer), 0)) > 0) {
        // 转发到客户端缓冲区
        conn->target_buffer.insert(conn->target_buffer.end(), buffer, buffer + bytes_read);
        bytes_read_total += bytes_read;
        ssize_t sent = send(conn->client_fd,
                    conn->target_buffer.data(),
                    conn->target_buffer.size(),
                    MSG_NOSIGNAL);
        if (sent > 0) {
            conn->target_buffer.erase(
                conn->target_buffer.begin(),
                conn->target_buffer.begin() + sent
            );
        }
        if (-1 == sent) {
            if (errno != EAGAIN) {
                std::ostringstream os;
                os << "Send target error: " << strerror(errno) << " (" << errno << ")";
                throw std::runtime_error(os.str());
            }
        }
        LOG_DEBUG("Received %zd bytes from target, sent %zd", bytes_read_total, sent);
    }

    LOG_DEBUG("Total bytes read from target: %zd,%zd, left: %zd", bytes_read_total, bytes_read, conn->target_buffer.size());
    
    if (bytes_read == 0) {
        conn->target_eof = true;
        LOG_DEBUG("Target EOF, closing client connection");
        // 这个地方关闭了链接，会导致client提前断开连接，后面缓冲区数据发送到client会失败，所以注释掉
        // shutdown(conn->client_fd, SHUT_WR);
    } else if (errno != EAGAIN) {
        std::ostringstream os;
        os << "Target read error: " << strerror(errno) << " (" << errno << ")";
        throw std::runtime_error(os.str());
    }
    
    // 尝试立即发送缓冲数据
    if (!conn->target_buffer.empty()) {
        ssize_t sent = send(conn->client_fd,
                          conn->target_buffer.data(),
                          conn->target_buffer.size(),
                          MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno != EAGAIN) {
                std::ostringstream os;
                os << "Send client error: " << strerror(errno) << " (" << errno << ")";
                throw std::runtime_error(os.str());
            }
        } else {
            conn->target_buffer.erase(
                conn->target_buffer.begin(),
                conn->target_buffer.begin() + sent
            );
        }
    }
    
    // 如果缓冲区仍有数据，注册写事件
    if (!conn->target_buffer.empty()) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = conn;
        LOG_DEBUG("Registering EPOLLOUT on client connection");
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->client_fd, &ev);
    }
    
    // 检查双向关闭
    // if ((conn->client_eof && conn->target_buffer.empty()) ||
    //     (conn->target_eof && conn->client_buffer.empty())) {
    //     LOG_DEBUG("Both directions closed, closing connection");
    //     close_connection(epoll_fd, conn);
    // }
    if (conn->client_eof && conn->target_eof) {
        LOG_DEBUG("Both directions closed, closing connection");
        close_connection(epoll_fd, conn);
    }
}
