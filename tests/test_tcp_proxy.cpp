// =============================================================================
// GatePulse — Test Suite for FR5: TCP Reverse Proxy & End-to-End Gateway
// =============================================================================
#include <iostream>
#include <string>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include "tcp_proxy.hpp"
#include "http_parser.hpp"

using namespace std;
using namespace gatepulse;

WinsockScope g_winsock_scope; // Global Winsock initialization for the test process

// ─── TEST 1: HTTP Request Parser & Serializer ────────────────────────────────
void test_http_request_parsing() {
    cout << "[TEST 1] HTTP request wire format parsing..." << endl;

    string raw = "POST /api/v1/checkout HTTP/1.1\r\n"
                 "Host: api.example.com\r\n"
                 "Authorization: Bearer token-xyz\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: 17\r\n"
                 "\r\n"
                 "{\"order_id\":9901}";

    auto req = HttpParser::parse_request(raw);
    assert(req.has_value());
    assert(req->method == "POST");
    assert(req->path == "/api/v1/checkout");
    assert(req->get_header("Host") == "api.example.com");
    assert(req->get_header("Authorization") == "Bearer token-xyz");
    assert(req->get_header("Content-Type") == "application/json");
    assert(req->body == "{\"order_id\":9901}");

    // Test serialization
    string serialized = HttpParser::serialize_request(*req);
    assert(serialized.find("POST /api/v1/checkout HTTP/1.1") != string::npos);
    assert(serialized.find("{\"order_id\":9901}") != string::npos);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 2: HTTP Response Parser & Serializer ───────────────────────────────
void test_http_response_parsing() {
    cout << "[TEST 2] HTTP response wire format parsing..." << endl;

    string raw = "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Server: MockBackend\r\n"
                 "Content-Length: 15\r\n"
                 "\r\n"
                 "{\"status\":\"ok\"}";

    auto res = HttpParser::parse_response(raw);
    assert(res.has_value());
    assert(res->status_code == 200);
    assert(res->status_message == "OK");
    assert(res->get_header("Content-Type") == "application/json");
    assert(res->get_header("Server") == "MockBackend");
    assert(res->body == "{\"status\":\"ok\"}");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 3: Malformed Request Detection ─────────────────────────────────────
void test_malformed_request() {
    cout << "[TEST 3] Malformed request rejection..." << endl;

    assert(!HttpParser::parse_request("").has_value());
    assert(!HttpParser::parse_request("JUST_GARBAGE_WITHOUT_NEWLINES").has_value());
    assert(!HttpParser::parse_request("INVALID\r\n\r\n").has_value());

    cout << "  -> Passed!" << endl;
}

// ─── TEST 4: Gateway 404 Route Not Found ─────────────────────────────────────
void test_gateway_route_not_found() {
    cout << "[TEST 4] Gateway 404 Not Found handling..." << endl;

    auto router = make_shared<TrieRouter>();
    router->add_route(HttpMethod::GET, "/api/v1/users", "users_handler");

    auto limiter = make_shared<RateLimiter>(100.0, 100.0);
    auto lb = make_shared<ConsistentHashLoadBalancer>(100);

    ReverseProxyGateway gw(18001, router, limiter, lb);

    HttpRequest req;
    req.method = "GET";
    req.path   = "/api/v1/non_existent";
    req.client_ip = "127.0.0.1";

    HttpResponse res = gw.process_request(req);
    assert(res.status_code == 404);
    assert(res.status_message == "Not Found");
    assert(res.get_header("Server") == "GatePulse/1.0");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 5: Gateway 503 No Upstream Available ──────────────────────────────
void test_gateway_no_upstream_available() {
    cout << "[TEST 5] Gateway 503 Service Unavailable when no healthy upstreams..." << endl;

    auto router = make_shared<TrieRouter>();
    router->add_route(HttpMethod::GET, "/api/v1/orders", "orders_handler");

    auto limiter = make_shared<RateLimiter>(100.0, 100.0);
    auto lb = make_shared<ConsistentHashLoadBalancer>(100); // Empty cluster

    ReverseProxyGateway gw(18002, router, limiter, lb);

    HttpRequest req;
    req.method = "GET";
    req.path   = "/api/v1/orders";
    req.client_ip = "127.0.0.1";

    HttpResponse res = gw.process_request(req);
    assert(res.status_code == 503);
    assert(res.status_message == "Service Unavailable");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 6: Gateway 502 Bad Gateway (Upstream Connection Refused) ───────────
void test_gateway_bad_gateway() {
    cout << "[TEST 6] Gateway 502 Bad Gateway (dead upstream server)..." << endl;

    auto router = make_shared<TrieRouter>();
    router->add_route(HttpMethod::GET, "/api/v1/products", "products_handler");

    auto limiter = make_shared<RateLimiter>(100.0, 100.0);
    auto lb = make_shared<ConsistentHashLoadBalancer>(100);

    // Register a port where nothing is listening (e.g. 59999)
    lb->add_server({"dead-node", "127.0.0.1", 59999, true, 1});

    ReverseProxyGateway gw(18003, router, limiter, lb);

    HttpRequest req;
    req.method = "GET";
    req.path   = "/api/v1/products";
    req.client_ip = "127.0.0.1";

    HttpResponse res = gw.process_request(req);
    assert(res.status_code == 502);
    assert(res.status_message == "Bad Gateway");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 7: Gateway 429 Rate Limiter Integration ────────────────────────────
void test_gateway_rate_limiting() {
    cout << "[TEST 7] Gateway 429 Too Many Requests over pipeline..." << endl;

    auto router = make_shared<TrieRouter>();
    router->add_route(HttpMethod::GET, "/api/v1/ping", "ping_handler");

    // Only 1 token allowed, 0.1/sec refill
    auto limiter = make_shared<RateLimiter>(1.0, 0.1);
    auto lb = make_shared<ConsistentHashLoadBalancer>(100);

    ReverseProxyGateway gw(18004, router, limiter, lb);

    HttpRequest req;
    req.method = "GET";
    req.path   = "/api/v1/ping";
    req.client_ip = "10.20.30.40";

    // 1st request -> Passes rate limit (fails later at 503 due to empty upstream, but not 429)
    HttpResponse res1 = gw.process_request(req);
    assert(res1.status_code == 503);

    // 2nd request -> Exhausted! Must return 429 Too Many Requests
    HttpResponse res2 = gw.process_request(req);
    assert(res2.status_code == 429);
    assert(res2.status_message == "Too Many Requests");
    assert(!res2.get_header("Retry-After").empty());

    cout << "  -> Passed!" << endl;
}

// ─── TEST 8: Live TCP Socket Reverse Proxy End-to-End ────────────────────────
void test_live_tcp_end_to_end_proxy() {
    cout << "[TEST 8] Live TCP Socket Reverse Proxy End-to-End..." << endl;

    const uint16_t UPSTREAM_PORT = 19091;
    const uint16_t GATEWAY_PORT  = 19090;

    // 1. Start a Mock Upstream HTTP Server on UPSTREAM_PORT
    atomic<bool> upstream_running = true;
    thread upstream_thread([UPSTREAM_PORT, &upstream_running]() {
        SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL opt = TRUE;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(UPSTREAM_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;

        bind(srv, (sockaddr*)&addr, sizeof(addr));
        listen(srv, 5);

        DWORD to = 2000;
        setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));

        while (upstream_running) {
            sockaddr_in client{};
            int len = sizeof(client);
            SOCKET cli = accept(srv, (sockaddr*)&client, &len);
            if (cli != INVALID_SOCKET) {
                SocketRAII s(cli);
                string req = s.receive(4096);
                if (!req.empty()) {
                    string payload = "{\"upstream\":\"backend-node-1\",\"result\":\"success\"}";
                    string res = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: " + to_string(payload.size()) + "\r\n"
                                 "\r\n" + payload;
                    s.send_all(res);
                }
            }
        }
        closesocket(srv);
    });

    // Let the mock upstream initialize
    this_thread::sleep_for(chrono::milliseconds(50));

    // 2. Configure Gateway (Router, RateLimiter, LoadBalancer)
    auto router = make_shared<TrieRouter>();
    router->add_route(HttpMethod::GET, "/api/v1/orders/:id", "get_order");

    auto limiter = make_shared<RateLimiter>(100.0, 100.0);
    auto lb = make_shared<ConsistentHashLoadBalancer>(100);
    lb->add_server({"backend-node-1", "127.0.0.1", UPSTREAM_PORT, true, 1});

    ReverseProxyGateway gateway(GATEWAY_PORT, router, limiter, lb);
    assert(gateway.start());

    this_thread::sleep_for(chrono::milliseconds(50));

    // 3. Connect real TCP Client to Gateway port
    SOCKET client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client_fd != INVALID_SOCKET);
    SocketRAII client_sock(client_fd);

    sockaddr_in gw_addr{};
    gw_addr.sin_family = AF_INET;
    gw_addr.sin_port = htons(GATEWAY_PORT);
    inet_pton(AF_INET, "127.0.0.1", &gw_addr.sin_addr);

    int conn_res = connect(client_fd, (sockaddr*)&gw_addr, sizeof(gw_addr));
    assert(conn_res != SOCKET_ERROR);

    // Send HTTP/1.1 request through Gateway to /api/v1/orders/8892
    string http_req = "GET /api/v1/orders/8892 HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "User-Agent: GatePulseTestClient/1.0\r\n"
                      "\r\n";
    assert(client_sock.send_all(http_req));

    // Receive relayed response from Gateway
    string http_res = client_sock.receive(8192);
    assert(!http_res.empty());

    // Verify response
    auto parsed_res = HttpParser::parse_response(http_res);
    assert(parsed_res.has_value());
    assert(parsed_res->status_code == 200);
    assert(parsed_res->status_message == "OK");
    assert(parsed_res->get_header("X-Proxy-By") == "GatePulse-L7");
    assert(parsed_res->get_header("X-Upstream-Backend") == "backend-node-1");
    assert(parsed_res->get_header("Server") == "GatePulse/1.0");
    assert(parsed_res->body.find("upstream_success") != string::npos ||
           parsed_res->body.find("backend-node-1") != string::npos);

    cout << "  -> Passed! (Received 200 OK from upstream via Gateway proxy relay)" << endl;

    // 4. Teardown
    gateway.stop();
    upstream_running = false;

    // Ping upstream to release accept blocking
    SOCKET ping_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in up_addr{};
    up_addr.sin_family = AF_INET;
    up_addr.sin_port = htons(UPSTREAM_PORT);
    inet_pton(AF_INET, "127.0.0.1", &up_addr.sin_addr);
    connect(ping_fd, (sockaddr*)&up_addr, sizeof(up_addr));
    closesocket(ping_fd);

    if (upstream_thread.joinable()) {
        upstream_thread.join();
    }
}

// ─── MAIN ────────────────────────────────────────────────────────────────────
int main() {
    cout << "========================================================" << endl;
    cout << "   GATEPULSE FR5: TCP Reverse Proxy & Gateway Tests     " << endl;
    cout << "========================================================" << endl;
    cout << endl;

    test_http_request_parsing();
    test_http_response_parsing();
    test_malformed_request();
    test_gateway_route_not_found();
    test_gateway_no_upstream_available();
    test_gateway_bad_gateway();
    test_gateway_rate_limiting();
    test_live_tcp_end_to_end_proxy();

    cout << endl;
    cout << "========================================================" << endl;
    cout << "   ALL 8 TESTS PASSED — FR5 VERIFIED!                   " << endl;
    cout << "========================================================" << endl;

    return 0;
}
