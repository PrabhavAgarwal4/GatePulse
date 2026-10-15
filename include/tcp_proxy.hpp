// =============================================================================
// GatePulse — High-Performance L7 API Gateway & Reverse Proxy
// File: include/tcp_proxy.hpp
// Milestone: FR5 — Winsock TCP Reverse Proxy & End-to-End Gateway Engine
// =============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <optional>
#include "trie_router.hpp"
#include "rate_limiter.hpp"
#include "load_balancer.hpp"
#include "middleware.hpp"
#include "http_parser.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace gatepulse {

// ─── Winsock Library RAII Manager ────────────────────────────────────────────
class WinsockScope {
public:
    WinsockScope() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::cerr << "[Winsock] WSAStartup failed" << std::endl;
        }
    }
    ~WinsockScope() {
        WSACleanup();
    }
};

// ─── RAII Socket Wrapper ─────────────────────────────────────────────────────
class SocketRAII {
public:
    SocketRAII() : fd_(INVALID_SOCKET) {}
    explicit SocketRAII(SOCKET fd) : fd_(fd) {}

    ~SocketRAII() {
        close();
    }

    // Move-only semantics to prevent socket descriptor duplication
    SocketRAII(SocketRAII&& other) noexcept : fd_(other.fd_) {
        other.fd_ = INVALID_SOCKET;
    }

    SocketRAII& operator=(SocketRAII&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = INVALID_SOCKET;
        }
        return *this;
    }

    SocketRAII(const SocketRAII&) = delete;
    SocketRAII& operator=(const SocketRAII&) = delete;

    SOCKET get() const { return fd_; }
    bool is_valid() const { return fd_ != INVALID_SOCKET; }

    void close() {
        if (fd_ != INVALID_SOCKET) {
            closesocket(fd_);
            fd_ = INVALID_SOCKET;
        }
    }

    // Send entire string buffer
    bool send_all(const std::string& data) const {
        if (fd_ == INVALID_SOCKET) return false;
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            int sent = send(fd_, data.data() + total_sent,
                            static_cast<int>(data.size() - total_sent), 0);
            if (sent <= 0) return false;
            total_sent += sent;
        }
        return true;
    }

    // Read available bytes up to max_bytes or until timeout
    std::string receive(size_t max_bytes = 8192) const {
        if (fd_ == INVALID_SOCKET) return "";
        std::vector<char> buf(max_bytes);
        int bytes_read = recv(fd_, buf.data(), static_cast<int>(max_bytes), 0);
        if (bytes_read <= 0) return "";
        return std::string(buf.data(), bytes_read);
    }

private:
    SOCKET fd_;
};

// ─── End-to-End L7 Reverse Proxy Gateway ──────────────────────────────────────
class ReverseProxyGateway {
public:
    ReverseProxyGateway(uint16_t port,
                        std::shared_ptr<TrieRouter> router,
                        std::shared_ptr<RateLimiter> rate_limiter,
                        std::shared_ptr<ConsistentHashLoadBalancer> load_balancer)
        : port_(port)
        , router_(std::move(router))
        , rate_limiter_(std::move(rate_limiter))
        , load_balancer_(std::move(load_balancer))
        , is_running_(false)
        , listen_fd_(INVALID_SOCKET)
    {
        setup_default_pipeline();
    }

    ~ReverseProxyGateway() {
        stop();
    }

    MiddlewarePipeline& pipeline() { return pipeline_; }

    // Start background listening TCP server
    bool start() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == INVALID_SOCKET) return false;

        // Enable SO_REUSEADDR to avoid TIME_WAIT port binding conflicts
        BOOL opt = TRUE;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port_);

        if (bind(listen_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            closesocket(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return false;
        }

        if (listen(listen_fd_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return false;
        }

        is_running_ = true;
        server_thread_ = std::thread(&ReverseProxyGateway::accept_loop, this);
        return true;
    }

    void stop() {
        if (is_running_) {
            is_running_ = false;
            if (listen_fd_ != INVALID_SOCKET) {
                closesocket(listen_fd_);
                listen_fd_ = INVALID_SOCKET;
            }
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
        }
    }

    bool is_running() const { return is_running_; }
    uint16_t port() const { return port_; }

    // Direct in-process execution for high-speed testing and simulation
    HttpResponse process_request(HttpRequest request) {
        HttpContext ctx;
        ctx.request = std::move(request);

        pipeline_.execute(ctx, [this](HttpContext& c) {
            handle_upstream_dispatch(c);
        });

        return ctx.response;
    }

private:
    uint16_t port_;
    std::shared_ptr<TrieRouter> router_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    std::shared_ptr<ConsistentHashLoadBalancer> load_balancer_;
    MiddlewarePipeline pipeline_;
    std::atomic<bool> is_running_;
    SOCKET listen_fd_;
    std::thread server_thread_;

    void setup_default_pipeline() {
        // Automatically chain RateLimiter and Security Headers
        if (rate_limiter_) {
            pipeline_.use(make_rate_limit_middleware(rate_limiter_));
        }
        pipeline_.use(make_security_headers_middleware());
    }

    void accept_loop() {
        while (is_running_) {
            sockaddr_in client_addr{};
            int client_len = sizeof(client_addr);
            SOCKET client_sock = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);

            if (client_sock == INVALID_SOCKET) {
                if (!is_running_) break;
                continue;
            }

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
            std::string client_ip = ip_str;

            // Handle client connection (in a detached worker or synchronously)
            std::thread([this, client_sock, client_ip]() {
                SocketRAII sock(client_sock);

                // Set 3-second receive timeout
                DWORD timeout_ms = 3000;
                setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

                std::string raw_req = sock.receive(16384);
                if (raw_req.empty()) return;

                auto maybe_req = HttpParser::parse_request(raw_req);
                if (!maybe_req.has_value()) {
                    HttpResponse bad_res;
                    bad_res.set_status(400, "Bad Request");
                    bad_res.body = "{\"error\":\"Malformed HTTP request\"}";
                    sock.send_all(HttpParser::serialize_response(bad_res));
                    return;
                }

                HttpRequest req = std::move(*maybe_req);
                req.client_ip = client_ip;

                HttpResponse res = process_request(std::move(req));
                std::string raw_res = HttpParser::serialize_response(res);
                sock.send_all(raw_res);
            }).detach();
        }
    }

    void handle_upstream_dispatch(HttpContext& ctx) {
        // 1. Convert string method to HttpMethod enum
        HttpMethod method = HttpMethod::GET;
        if (ctx.request.method == "POST")          method = HttpMethod::POST;
        else if (ctx.request.method == "PUT")     method = HttpMethod::PUT;
        else if (ctx.request.method == "DELETE")  method = HttpMethod::DELETE_METHOD;
        else if (ctx.request.method == "PATCH")   method = HttpMethod::PATCH;
        else if (ctx.request.method == "OPTIONS") method = HttpMethod::OPTIONS;

        // 2. Match Route via Trie Router
        auto route_match = router_->match(method, ctx.request.path);
        if (!route_match.has_value()) {
            ctx.response.set_status(404, "Not Found");
            ctx.response.set_header("Content-Type", "application/json");
            ctx.response.body = "{\"error\":\"Route not found on gateway\"}";
            return;
        }

        ctx.request.params = route_match->params;

        // 3. Select Upstream Server via Consistent Hash Load Balancer
        std::string routing_key = ctx.request.client_ip.empty() ? ctx.request.path : ctx.request.client_ip;
        auto upstream = load_balancer_->get_server(routing_key);

        if (!upstream.has_value()) {
            ctx.response.set_status(503, "Service Unavailable");
            ctx.response.set_header("Content-Type", "application/json");
            ctx.response.body = "{\"error\":\"No healthy upstream backends available\"}";
            return;
        }

        // 4. Connect to upstream TCP socket & forward HTTP request
        SOCKET up_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (up_fd == INVALID_SOCKET) {
            ctx.response.set_status(502, "Bad Gateway");
            ctx.response.body = "{\"error\":\"Failed to create upstream socket\"}";
            return;
        }
        SocketRAII up_sock(up_fd);

        DWORD timeout_ms = 2000;
        setsockopt(up_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
        setsockopt(up_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

        sockaddr_in up_addr{};
        up_addr.sin_family = AF_INET;
        up_addr.sin_port = htons(upstream->port);
        inet_pton(AF_INET, upstream->host.c_str(), &up_addr.sin_addr);

        if (connect(up_fd, (sockaddr*)&up_addr, sizeof(up_addr)) == SOCKET_ERROR) {
            ctx.response.set_status(502, "Bad Gateway");
            ctx.response.body = "{\"error\":\"Upstream connection refused\"}";
            return;
        }

        // Forward serialized HTTP request to upstream
        std::string serialized_req = HttpParser::serialize_request(ctx.request);
        if (!up_sock.send_all(serialized_req)) {
            ctx.response.set_status(502, "Bad Gateway");
            ctx.response.body = "{\"error\":\"Failed to send payload upstream\"}";
            return;
        }

        // Receive response from upstream
        std::string raw_res = up_sock.receive(65536);
        if (raw_res.empty()) {
            ctx.response.set_status(502, "Bad Gateway");
            ctx.response.body = "{\"error\":\"Empty response from upstream\"}";
            return;
        }

        auto maybe_res = HttpParser::parse_response(raw_res);
        if (!maybe_res.has_value()) {
            // Forward raw body if parser was unable to parse status line
            ctx.response.set_status(200, "OK");
            ctx.response.body = raw_res;
        } else {
            ctx.response = std::move(*maybe_res);
        }

        // Inject Gateway identification header
        ctx.response.set_header("X-Proxy-By", "GatePulse-L7");
        ctx.response.set_header("X-Upstream-Backend", upstream->id);
    }
};

}  // namespace gatepulse
