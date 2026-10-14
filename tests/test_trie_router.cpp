// =============================================================================
// GatePulse — Test Suite for FR1: Trie Route Matching Engine
// =============================================================================
#include <iostream>
#include <string>
#include <cassert>
#include "trie_router.hpp"

using namespace std;
using namespace gatepulse;

// ─── TEST 1: Basic Static Route Matching ─────────────────────────────────────
void test_static_route_matching() {
    cout << "[TEST 1] Static route matching..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/api/health", "health_handler");
    router.add_route(HttpMethod::GET, "/api/version", "version_handler");
    router.add_route(HttpMethod::GET, "/api/users/list", "list_users");

    auto m1 = router.match(HttpMethod::GET, "/api/health");
    assert(m1.has_value());
    assert(m1->handler_name == "health_handler");
    assert(m1->params.empty());

    auto m2 = router.match(HttpMethod::GET, "/api/version");
    assert(m2.has_value());
    assert(m2->handler_name == "version_handler");

    auto m3 = router.match(HttpMethod::GET, "/api/users/list");
    assert(m3.has_value());
    assert(m3->handler_name == "list_users");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 2: Parameterized Route Matching ────────────────────────────────────
void test_param_route_matching() {
    cout << "[TEST 2] Parameterized route matching..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/api/users/:id", "get_user");
    router.add_route(HttpMethod::GET, "/api/users/:id/profile", "get_profile");

    auto m1 = router.match(HttpMethod::GET, "/api/users/42");
    assert(m1.has_value());
    assert(m1->handler_name == "get_user");
    assert(m1->params.at("id") == "42");

    auto m2 = router.match(HttpMethod::GET, "/api/users/alice/profile");
    assert(m2.has_value());
    assert(m2->handler_name == "get_profile");
    assert(m2->params.at("id") == "alice");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 3: Multi-Parameter Extraction ──────────────────────────────────────
void test_multi_param_extraction() {
    cout << "[TEST 3] Multi-parameter extraction..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/api/:version/users/:id", "versioned_user");

    auto m1 = router.match(HttpMethod::GET, "/api/v2/users/99");
    assert(m1.has_value());
    assert(m1->handler_name == "versioned_user");
    assert(m1->params.at("version") == "v2");
    assert(m1->params.at("id") == "99");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 4: Wildcard Catch-All ──────────────────────────────────────────────
void test_wildcard_route() {
    cout << "[TEST 4] Wildcard catch-all route..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/static/*", "static_file_handler");

    auto m1 = router.match(HttpMethod::GET, "/static/css/app.css");
    assert(m1.has_value());
    assert(m1->handler_name == "static_file_handler");
    assert(m1->params.at("*") == "css/app.css");

    auto m2 = router.match(HttpMethod::GET, "/static/js/bundle.min.js");
    assert(m2.has_value());
    assert(m2->params.at("*") == "js/bundle.min.js");

    // Single segment after wildcard prefix
    auto m3 = router.match(HttpMethod::GET, "/static/favicon.ico");
    assert(m3.has_value());
    assert(m3->params.at("*") == "favicon.ico");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 5: Static vs Param Priority ────────────────────────────────────────
// When both /users/admin (static) and /users/:id (param) exist,
// the static route MUST win for the path "/users/admin".
void test_static_over_param_priority() {
    cout << "[TEST 5] Static vs parameterized priority..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/users/:id", "get_user_by_id");
    router.add_route(HttpMethod::GET, "/users/admin", "admin_panel");

    // "/users/admin" → must match static "admin_panel", NOT param "get_user_by_id"
    auto m1 = router.match(HttpMethod::GET, "/users/admin");
    assert(m1.has_value());
    assert(m1->handler_name == "admin_panel");
    assert(m1->params.empty());  // No params captured — it was a static match

    // "/users/bob" → must match param route since "bob" is not a static child
    auto m2 = router.match(HttpMethod::GET, "/users/bob");
    assert(m2.has_value());
    assert(m2->handler_name == "get_user_by_id");
    assert(m2->params.at("id") == "bob");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 6: HTTP Method Differentiation ─────────────────────────────────────
// GET /api/users and POST /api/users must resolve to different handlers.
void test_http_method_differentiation() {
    cout << "[TEST 6] HTTP method differentiation on same path..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET,  "/api/users", "list_users");
    router.add_route(HttpMethod::POST, "/api/users", "create_user");
    router.add_route(HttpMethod::DELETE_METHOD, "/api/users/:id", "delete_user");

    auto m1 = router.match(HttpMethod::GET, "/api/users");
    assert(m1.has_value());
    assert(m1->handler_name == "list_users");

    auto m2 = router.match(HttpMethod::POST, "/api/users");
    assert(m2.has_value());
    assert(m2->handler_name == "create_user");

    auto m3 = router.match(HttpMethod::DELETE_METHOD, "/api/users/42");
    assert(m3.has_value());
    assert(m3->handler_name == "delete_user");
    assert(m3->params.at("id") == "42");

    // PUT on /api/users is NOT registered → must return nullopt
    auto m4 = router.match(HttpMethod::PUT, "/api/users");
    assert(!m4.has_value());

    cout << "  -> Passed!" << endl;
}

// ─── TEST 7: No Match Returns nullopt ────────────────────────────────────────
void test_no_match() {
    cout << "[TEST 7] Unregistered routes return nullopt..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/api/health", "health_handler");

    // Completely wrong path
    assert(!router.match(HttpMethod::GET, "/nonexistent").has_value());

    // Partial prefix match — must NOT match
    assert(!router.match(HttpMethod::GET, "/api").has_value());

    // Path is longer than any registered route
    assert(!router.match(HttpMethod::GET, "/api/health/extra").has_value());

    // Correct path, wrong method
    assert(!router.match(HttpMethod::POST, "/api/health").has_value());

    cout << "  -> Passed!" << endl;
}

// ─── TEST 8: Trailing Slash Normalization ────────────────────────────────────
void test_trailing_slash() {
    cout << "[TEST 8] Trailing slash normalization..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/api/health", "health_handler");

    // With trailing slash — should still match since split_path ignores empty segments
    auto m1 = router.match(HttpMethod::GET, "/api/health/");
    assert(m1.has_value());
    assert(m1->handler_name == "health_handler");

    cout << "  -> Passed!" << endl;
}

// ─── TEST 9: Route Count ────────────────────────────────────────────────────
void test_route_count() {
    cout << "[TEST 9] Route count tracking..." << endl;
    TrieRouter router;

    assert(router.route_count() == 0);

    router.add_route(HttpMethod::GET,  "/a", "ha");
    router.add_route(HttpMethod::POST, "/a", "hb");
    router.add_route(HttpMethod::GET,  "/b", "hc");

    assert(router.route_count() == 3);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 10: Root Path Matching ─────────────────────────────────────────────
void test_root_path() {
    cout << "[TEST 10] Root path '/' matching..." << endl;
    TrieRouter router;

    router.add_route(HttpMethod::GET, "/", "root_handler");

    auto m1 = router.match(HttpMethod::GET, "/");
    assert(m1.has_value());
    assert(m1->handler_name == "root_handler");
    assert(m1->params.empty());

    cout << "  -> Passed!" << endl;
}

// ─── TEST 11: Stress — Large Route Table ─────────────────────────────────────
void test_large_route_table() {
    cout << "[TEST 11] Stress test with 10,000 routes..." << endl;
    TrieRouter router;

    // Register 10,000 unique routes: /route/0, /route/1, ..., /route/9999
    for (int i = 0; i < 10000; ++i) {
        router.add_route(HttpMethod::GET,
                         "/route/" + to_string(i),
                         "handler_" + to_string(i));
    }

    assert(router.route_count() == 10000);

    // Verify first, middle, and last
    auto m1 = router.match(HttpMethod::GET, "/route/0");
    assert(m1.has_value() && m1->handler_name == "handler_0");

    auto m2 = router.match(HttpMethod::GET, "/route/5000");
    assert(m2.has_value() && m2->handler_name == "handler_5000");

    auto m3 = router.match(HttpMethod::GET, "/route/9999");
    assert(m3.has_value() && m3->handler_name == "handler_9999");

    // Non-existent route among 10,000
    assert(!router.match(HttpMethod::GET, "/route/10000").has_value());

    cout << "  -> Passed!" << endl;
}

// ─── TEST 12: Complex Mixed Routes ──────────────────────────────────────────
// Simulates a real API gateway routing table with mixed route types.
void test_real_world_routing_table() {
    cout << "[TEST 12] Real-world mixed routing table..." << endl;
    TrieRouter router;

    // Static health & metadata endpoints
    router.add_route(HttpMethod::GET, "/health", "health_check");
    router.add_route(HttpMethod::GET, "/metrics", "prometheus_metrics");

    // RESTful user API
    router.add_route(HttpMethod::GET,    "/api/v1/users",     "list_users");
    router.add_route(HttpMethod::POST,   "/api/v1/users",     "create_user");
    router.add_route(HttpMethod::GET,    "/api/v1/users/:id", "get_user");
    router.add_route(HttpMethod::PUT,    "/api/v1/users/:id", "update_user");
    router.add_route(HttpMethod::DELETE_METHOD, "/api/v1/users/:id", "delete_user");

    // Nested resources — note: :id is reused at the users level because the Trie
    // shares a single param node per level. This is the same convention used by
    // Gin (Go) and httprouter: a param name at a given depth must be consistent.
    router.add_route(HttpMethod::GET, "/api/v1/users/:id/posts/:pid", "get_user_post");

    // Static file catch-all
    router.add_route(HttpMethod::GET, "/assets/*", "serve_static");

    // Verify each route type
    auto r1 = router.match(HttpMethod::GET, "/health");
    assert(r1.has_value() && r1->handler_name == "health_check");

    auto r2 = router.match(HttpMethod::POST, "/api/v1/users");
    assert(r2.has_value() && r2->handler_name == "create_user");

    auto r3 = router.match(HttpMethod::PUT, "/api/v1/users/42");
    assert(r3.has_value() && r3->handler_name == "update_user");
    assert(r3->params.at("id") == "42");

    auto r4 = router.match(HttpMethod::GET, "/api/v1/users/7/posts/101");
    assert(r4.has_value() && r4->handler_name == "get_user_post");
    assert(r4->params.at("id") == "7");
    assert(r4->params.at("pid") == "101");

    auto r5 = router.match(HttpMethod::GET, "/assets/img/logo.png");
    assert(r5.has_value() && r5->handler_name == "serve_static");
    assert(r5->params.at("*") == "img/logo.png");

    cout << "  -> Passed!" << endl;
}

// ─── MAIN ────────────────────────────────────────────────────────────────────
int main() {
    cout << "========================================================" << endl;
    cout << "   GATEPULSE FR1: Trie Route Matching — Test Suite      " << endl;
    cout << "========================================================" << endl;
    cout << endl;

    test_static_route_matching();
    test_param_route_matching();
    test_multi_param_extraction();
    test_wildcard_route();
    test_static_over_param_priority();
    test_http_method_differentiation();
    test_no_match();
    test_trailing_slash();
    test_route_count();
    test_root_path();
    test_large_route_table();
    test_real_world_routing_table();

    cout << endl;
    cout << "========================================================" << endl;
    cout << "   ALL 12 TESTS PASSED — FR1 VERIFIED!                  " << endl;
    cout << "========================================================" << endl;

    return 0;
}
