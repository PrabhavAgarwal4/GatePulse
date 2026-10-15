// =============================================================================
// GatePulse — Standalone High-Performance L7 API Gateway Server
// File: src/main.cpp
// =============================================================================
#include <iostream>
#include <string>
#include <memory>
#include <csignal>
#include <atomic>
#include "tcp_proxy.hpp"

using namespace std;
using namespace gatepulse;

atomic<bool> g_shutdown(false);

void signal_handler(int) {
    cout << "\n[GatePulse] Graceful shutdown initiated..." << endl;
    g_shutdown = true;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    WinsockScope winsock;

    uint16_t port = 8080;
    if (argc > 1) {
        port = static_cast<uint16_t>(stoi(argv[1]));
    }

    cout << "================================================================" << endl;
    cout << "          GATEPULSE — HIGH-PERFORMANCE L7 API GATEWAY           " << endl;
    cout << "                    Modern C++20 / Winsock                      " << endl;
    cout << "================================================================" << endl;

    // 1. Initialize Prefix Trie Router
    auto router = make_shared<TrieRouter>();
    router->add_route(HttpMethod::GET, "/health", "health_handler");
    router->add_route(HttpMethod::GET, "/api/v1/users/:id", "users_service");
    router->add_route(HttpMethod::POST, "/api/v1/users", "create_user");
    router->add_route(HttpMethod::GET, "/static/*", "static_assets");

    // 2. Initialize Token Bucket Rate Limiter (100 req burst, 20 req/sec sustained)
    auto rate_limiter = make_shared<RateLimiter>(100.0, 20.0);

    // 3. Initialize Consistent Hash Load Balancer (150 virtual nodes)
    auto load_balancer = make_shared<ConsistentHashLoadBalancer>(150);
    load_balancer->add_server({"upstream-alpha", "127.0.0.1", 9001, true, 1});
    load_balancer->add_server({"upstream-beta",  "127.0.0.1", 9002, true, 1});

    // 4. Instantiate Gateway
    ReverseProxyGateway gateway(port, router, rate_limiter, load_balancer);

    // 5. Configure Middleware Pipeline
    gateway.pipeline().use(make_logging_middleware());
    gateway.pipeline().use(make_cors_middleware());

    if (!gateway.start()) {
        cerr << "[GatePulse] Error: Failed to bind to port " << port << endl;
        return 1;
    }

    cout << "[GatePulse] Listening on http://0.0.0.0:" << port << endl;
    cout << "[GatePulse] Trie routes registered : " << router->route_count() << endl;
    cout << "[GatePulse] Load balancer ring size : " << load_balancer->ring_points_count() << " v-nodes" << endl;
    cout << "[GatePulse] Press Ctrl+C to terminate..." << endl;
    cout << "----------------------------------------------------------------" << endl;

    // Simple event loop waiting for shutdown signal
    while (!g_shutdown) {
        this_thread::sleep_for(chrono::milliseconds(200));
    }

    gateway.stop();
    cout << "[GatePulse] Server stopped cleanly. Bye!" << endl;
    return 0;
}
