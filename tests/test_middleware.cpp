// =============================================================================
// GatePulse — Test Suite for FR4: Middleware Pipeline & Chain of Responsibility
// =============================================================================
#include <iostream>
#include <string>
#include <cassert>
#include <vector>
#include <memory>
#include "middleware.hpp"
#include "rate_limiter.hpp"

using namespace std;
using namespace gatepulse;

// ─── TEST 1: Onion Execution Order (Pre-processing -> Inner -> Post-processing) ───
void test_onion_execution_order() {
    cout << "[TEST 1] Onion execution order (FIFO in, LIFO out)..." << endl;
    MiddlewarePipeline pipeline;
    vector<string> trace;

    pipeline.use([&trace](HttpContext& ctx, NextFunction next) {
        trace.push_back("M1_PRE");
        next();
        trace.push_back("M1_POST");
    });

    pipeline.use([&trace](HttpContext& ctx, NextFunction next) {
        trace.push_back("M2_PRE");
        next();
        trace.push_back("M2_POST");
    });

    HttpContext ctx;
    ctx.request.method = "GET";
    ctx.request.path = "/test";

    pipeline.execute(ctx, [&trace](HttpContext& c) {
        trace.push_back("HANDLER");
        c.response.set_status(200, "OK");
    });

    // Verify strict onion ordering
    assert(trace.size() == 5);
    assert(trace[0] == "M1_PRE");
    assert(trace[1] == "M2_PRE");
    assert(trace[2] == "HANDLER");
    assert(trace[3] == "M2_POST");
    assert(trace[4] == "M1_POST");
    assert(ctx.response.status_code == 200);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 2: Short-Circuiting on Authentication Failure ──────────────────────
void test_auth_short_circuit() {
    cout << "[TEST 2] Short-circuiting on auth failure (skipping downstream)..." << endl;
    MiddlewarePipeline pipeline;
    bool handler_called = false;
    bool downstream_mw_called = false;

    // Auth middleware requiring token "secret123"
    pipeline.use(make_auth_middleware("secret123"));

    pipeline.use([&downstream_mw_called](HttpContext& ctx, NextFunction next) {
        downstream_mw_called = true;
        next();
    });

    HttpContext ctx;
    ctx.request.method = "GET";
    ctx.request.path = "/api/secure-data";
    ctx.request.set_header("Authorization", "Bearer wrong-token");

    pipeline.execute(ctx, [&handler_called](HttpContext& c) {
        handler_called = true;
    });

    // Verification: Neither downstream middleware nor handler should execute!
    assert(!downstream_mw_called);
    assert(!handler_called);
    assert(ctx.response.status_code == 401);
    assert(ctx.response.status_message == "Unauthorized");

    // Now send valid token: must pass!
    HttpContext valid_ctx;
    valid_ctx.request.method = "GET";
    valid_ctx.request.path = "/api/secure-data";
    valid_ctx.request.set_header("Authorization", "Bearer secret123");

    pipeline.execute(valid_ctx, [&handler_called](HttpContext& c) {
        handler_called = true;
        c.response.set_status(200, "OK");
        c.response.body = "{\"data\":\"confidential\"}";
    });

    assert(downstream_mw_called);
    assert(handler_called);
    assert(valid_ctx.response.status_code == 200);
    assert(valid_ctx.get_attribute("authenticated") == "true");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 3: Cross-Middleware Attribute Passing ──────────────────────────────
void test_context_attributes() {
    cout << "[TEST 3] Cross-middleware context attribute passing..." << endl;
    MiddlewarePipeline pipeline;

    // Middleware A: Extracts client ID and stamps user metadata
    pipeline.use([](HttpContext& ctx, NextFunction next) {
        ctx.set_attribute("tenant_id", "tenant-alpha");
        ctx.set_attribute("role", "admin");
        next();
    });

    // Middleware B: Reads metadata set by Middleware A
    pipeline.use([](HttpContext& ctx, NextFunction next) {
        assert(ctx.get_attribute("tenant_id") == "tenant-alpha");
        assert(ctx.get_attribute("role") == "admin");
        ctx.set_attribute("tier", "enterprise");
        next();
    });

    HttpContext ctx;
    pipeline.execute(ctx, [](HttpContext& c) {
        assert(c.get_attribute("tier") == "enterprise");
        c.response.set_status(200, "OK");
    });

    assert(ctx.response.status_code == 200);
    cout << "  -> Passed!" << endl;
}

// ─── TEST 4: Rate Limiter Middleware Integration ─────────────────────────────
void test_rate_limiter_middleware() {
    cout << "[TEST 4] Rate limiter middleware integration (429 short-circuit)..." << endl;
    // Allow 2 requests burst, 1 token/sec refill
    auto limiter = make_shared<RateLimiter>(2.0, 1.0);
    MiddlewarePipeline pipeline;
    pipeline.use(make_rate_limit_middleware(limiter));

    int handler_count = 0;
    auto handler = [&handler_count](HttpContext& c) {
        handler_count++;
        c.response.set_status(200, "OK");
    };

    HttpContext ctx1;
    ctx1.request.client_ip = "192.168.1.100";
    pipeline.execute(ctx1, handler);
    assert(ctx1.response.status_code == 200);

    HttpContext ctx2;
    ctx2.request.client_ip = "192.168.1.100";
    pipeline.execute(ctx2, handler);
    assert(ctx2.response.status_code == 200);

    // 3rd request from same IP -> Must be rate-limited (HTTP 429)
    HttpContext ctx3;
    ctx3.request.client_ip = "192.168.1.100";
    pipeline.execute(ctx3, handler);
    assert(ctx3.response.status_code == 429);
    assert(ctx3.response.status_message == "Too Many Requests");
    assert(!ctx3.response.get_header("Retry-After").empty());
    assert(handler_count == 2); // Handler was NOT invoked for 3rd request!

    // Different IP -> Allowed!
    HttpContext ctx_other;
    ctx_other.request.client_ip = "192.168.1.200";
    pipeline.execute(ctx_other, handler);
    assert(ctx_other.response.status_code == 200);
    assert(handler_count == 3);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 5: CORS Preflight & Header Mutation ────────────────────────────────
void test_cors_middleware() {
    cout << "[TEST 5] CORS preflight OPTIONS short-circuit & header injection..." << endl;
    MiddlewarePipeline pipeline;
    pipeline.use(make_cors_middleware("https://myapp.com"));

    bool handler_called = false;

    // Case 1: OPTIONS preflight check -> Must short-circuit with 204 No Content
    HttpContext preflight_ctx;
    preflight_ctx.request.method = "OPTIONS";
    preflight_ctx.request.path = "/api/v1/resource";

    pipeline.execute(preflight_ctx, [&handler_called](HttpContext& c) {
        handler_called = true;
    });

    assert(!handler_called); // Handler must NOT be called for preflight OPTIONS!
    assert(preflight_ctx.response.status_code == 204);
    assert(preflight_ctx.response.get_header("Access-Control-Allow-Origin") == "https://myapp.com");

    // Case 2: Standard GET request -> Must inject CORS header and execute handler
    HttpContext get_ctx;
    get_ctx.request.method = "GET";
    get_ctx.request.path = "/api/v1/resource";

    pipeline.execute(get_ctx, [&handler_called](HttpContext& c) {
        handler_called = true;
        c.response.set_status(200, "OK");
    });

    assert(handler_called);
    assert(get_ctx.response.status_code == 200);
    assert(get_ctx.response.get_header("Access-Control-Allow-Origin") == "https://myapp.com");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 6: Logging & Security Headers Injection ────────────────────────────
void test_logging_and_security_headers() {
    cout << "[TEST 6] Logging latency & Security headers injection..." << endl;
    vector<string> logs;
    MiddlewarePipeline pipeline;

    pipeline.use(make_logging_middleware(&logs));
    pipeline.use(make_security_headers_middleware());

    HttpContext ctx;
    ctx.request.method = "POST";
    ctx.request.path = "/api/login";

    pipeline.execute(ctx, [](HttpContext& c) {
        c.response.set_status(200, "OK");
        c.response.body = "{\"token\":\"xyz\"}";
    });

    // Check security headers injected
    assert(ctx.response.get_header("X-Content-Type-Options") == "nosniff");
    assert(ctx.response.get_header("X-Frame-Options") == "DENY");
    assert(ctx.response.get_header("Server") == "GatePulse/1.0");

    // Check latency header injected
    assert(!ctx.response.get_header("X-Response-Time-Ms").empty());

    // Check log sink captured request
    assert(logs.size() == 1);
    assert(logs[0].find("POST /api/login -> 200") != string::npos);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 7: Empty Pipeline Direct Handler Execution ─────────────────────────
void test_empty_pipeline() {
    cout << "[TEST 7] Empty pipeline executing handler directly..." << endl;
    MiddlewarePipeline pipeline;
    assert(pipeline.size() == 0);

    bool reached = false;
    HttpContext ctx;
    pipeline.execute(ctx, [&reached](HttpContext& c) {
        reached = true;
        c.response.set_status(200, "OK");
    });

    assert(reached);
    assert(ctx.response.status_code == 200);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 8: Multi-Stage Chained Pipeline ───────────────────────────────────
void test_multi_stage_pipeline() {
    cout << "[TEST 8] Multi-stage chain (5 distinct middlewares in series)..." << endl;
    MiddlewarePipeline pipeline;

    auto limiter = make_shared<RateLimiter>(100.0, 100.0);

    pipeline.use(make_logging_middleware());
    pipeline.use(make_security_headers_middleware());
    pipeline.use(make_cors_middleware());
    pipeline.use(make_rate_limit_middleware(limiter));
    pipeline.use(make_auth_middleware("admin-key"));

    HttpContext ctx;
    ctx.request.method = "GET";
    ctx.request.path = "/api/admin/metrics";
    ctx.request.client_ip = "127.0.0.1";
    ctx.request.set_header("Authorization", "Bearer admin-key");

    bool executed = false;
    pipeline.execute(ctx, [&executed](HttpContext& c) {
        executed = true;
        c.response.set_status(200, "OK");
        c.response.body = "{\"status\":\"healthy\"}";
    });

    assert(executed);
    assert(ctx.response.status_code == 200);
    assert(ctx.response.get_header("Access-Control-Allow-Origin") == "*");
    assert(ctx.response.get_header("X-Frame-Options") == "DENY");
    assert(!ctx.response.get_header("X-Response-Time-Ms").empty());

    cout << "  -> Passed!" << endl;
}

// ─── TEST 9: High-Throughput Stress Test ─────────────────────────────────────
void test_pipeline_throughput_stress() {
    cout << "[TEST 9] High throughput: 50,000 requests through pipeline..." << endl;
    MiddlewarePipeline pipeline;

    pipeline.use([](HttpContext& ctx, NextFunction next) {
        ctx.request.set_header("X-Stage-1", "Processed");
        next();
        ctx.response.set_header("X-Stage-1-Out", "Done");
    });

    pipeline.use([](HttpContext& ctx, NextFunction next) {
        ctx.request.set_header("X-Stage-2", "Validated");
        next();
    });

    const int TOTAL_REQS = 50000;
    for (int i = 0; i < TOTAL_REQS; ++i) {
        HttpContext ctx;
        ctx.request.method = "GET";
        ctx.request.path = "/item/" + to_string(i);

        pipeline.execute(ctx, [](HttpContext& c) {
            c.response.set_status(200, "OK");
        });

        assert(ctx.response.status_code == 200);
        assert(ctx.response.get_header("X-Stage-1-Out") == "Done");
    }

    cout << "  -> Passed! (50,000 pipeline executions completed)" << endl;
}

// ─── MAIN ────────────────────────────────────────────────────────────────────
int main() {
    cout << "========================================================" << endl;
    cout << "   GATEPULSE FR4: Middleware Pipeline Tests             " << endl;
    cout << "========================================================" << endl;
    cout << endl;

    test_onion_execution_order();
    test_auth_short_circuit();
    test_context_attributes();
    test_rate_limiter_middleware();
    test_cors_middleware();
    test_logging_and_security_headers();
    test_empty_pipeline();
    test_multi_stage_pipeline();
    test_pipeline_throughput_stress();

    cout << endl;
    cout << "========================================================" << endl;
    cout << "   ALL 9 TESTS PASSED — FR4 VERIFIED!                   " << endl;
    cout << "========================================================" << endl;

    return 0;
}
