// =============================================================================
// GatePulse — Test Suite for FR2: Token Bucket Rate Limiting
// =============================================================================
//
// Testing strategy for time-dependent code:
//   We use HIGH refill rates (10,000 tokens/sec) and SHORT sleeps (5-10ms)
//   so tests finish in milliseconds while still verifying time-based behavior.
//   At 10,000 tokens/sec, a 5ms sleep refills ~50 tokens — plenty of margin
//   even if the OS scheduler overshoots by 2-3ms.
// =============================================================================
#include <iostream>
#include <string>
#include <cassert>
#include <thread>
#include <chrono>
#include <cmath>
#include "rate_limiter.hpp"

using namespace std;
using namespace gatepulse;

// ─── TEST 1: Basic Allow and Deny ────────────────────────────────────────────
// Consume all tokens from a bucket, then verify the next request is denied.
void test_basic_allow_deny() {
    cout << "[TEST 1] Basic allow and deny..." << endl;
    TokenBucket bucket(3.0, 1.0);  // 3 tokens max, 1 token/sec refill

    // Bucket starts full — first 3 requests must be allowed
    auto r1 = bucket.try_consume();
    assert(r1.allowed == true);
    assert(r1.tokens_remaining >= 1.99);  // ~2.0 remaining

    auto r2 = bucket.try_consume();
    assert(r2.allowed == true);

    auto r3 = bucket.try_consume();
    assert(r3.allowed == true);

    // 4th request — bucket is empty, must be denied
    auto r4 = bucket.try_consume();
    assert(r4.allowed == false);
    assert(r4.retry_after_secs > 0.0);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 2: Refill Over Time ────────────────────────────────────────────────
// Drain the bucket, wait for refill, then verify requests are allowed again.
void test_refill_over_time() {
    cout << "[TEST 2] Refill after time elapses..." << endl;
    TokenBucket bucket(5.0, 10000.0);  // 5 max, 10,000/sec (fast for testing)

    // Drain the bucket completely
    for (int i = 0; i < 5; ++i) {
        assert(bucket.try_consume().allowed);
    }
    assert(!bucket.try_consume().allowed);  // Empty

    // Sleep 5ms → ~50 tokens refilled, capped at 5
    this_thread::sleep_for(chrono::milliseconds(5));

    // Should now be refilled (up to max)
    auto r = bucket.try_consume();
    assert(r.allowed == true);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 3: Token Cap (No Overflow) ─────────────────────────────────────────
// After a long idle, tokens should NOT exceed max_tokens.
void test_no_overflow() {
    cout << "[TEST 3] Token cap — no overflow beyond max_tokens..." << endl;
    TokenBucket bucket(5.0, 10000.0);  // 5 max, 10,000/sec

    // Sleep long enough to refill way beyond capacity
    this_thread::sleep_for(chrono::milliseconds(10));  // Would refill ~100

    // Consume 5 — should work (bucket is full = 5)
    for (int i = 0; i < 5; ++i) {
        assert(bucket.try_consume().allowed);
    }

    // 6th should fail — bucket was capped at 5, not 100+
    assert(!bucket.try_consume().allowed);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 4: Burst Then Throttle ─────────────────────────────────────────────
// A client can burst up to max_tokens immediately, then must wait.
void test_burst_then_throttle() {
    cout << "[TEST 4] Burst capacity then throttle..." << endl;
    TokenBucket bucket(10.0, 2.0);  // 10 max, 2/sec

    // Burst: consume all 10 tokens rapidly
    for (int i = 0; i < 10; ++i) {
        auto r = bucket.try_consume();
        assert(r.allowed);
    }

    // Now throttled
    auto denied = bucket.try_consume();
    assert(!denied.allowed);
    // retry_after should be ~0.5 sec (need 1 token, rate is 2/sec)
    assert(denied.retry_after_secs > 0.3);
    assert(denied.retry_after_secs < 1.0);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 5: Multi-Token Consumption ─────────────────────────────────────────
// Consuming multiple tokens in a single request (e.g., bulk API operations).
void test_multi_token_consume() {
    cout << "[TEST 5] Multi-token consumption..." << endl;
    TokenBucket bucket(10.0, 1.0);  // 10 max, 1/sec

    // Consume 7 tokens at once
    auto r1 = bucket.try_consume(7.0);
    assert(r1.allowed);
    assert(r1.tokens_remaining >= 2.99 && r1.tokens_remaining <= 3.01);

    // 4 more should fail (only ~3 remaining)
    auto r2 = bucket.try_consume(4.0);
    assert(!r2.allowed);

    // But 2 should succeed
    auto r3 = bucket.try_consume(2.0);
    assert(r3.allowed);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 6: Retry-After Calculation ─────────────────────────────────────────
// When denied, the retry_after_secs should accurately reflect the wait time.
void test_retry_after_calculation() {
    cout << "[TEST 6] Retry-After header calculation..." << endl;
    TokenBucket bucket(1.0, 10.0);  // 1 max, 10/sec

    // Consume the only token
    assert(bucket.try_consume().allowed);

    // Denied — need 1 token at 10/sec → retry_after ≈ 0.1 sec
    auto denied = bucket.try_consume();
    assert(!denied.allowed);
    assert(denied.retry_after_secs > 0.05);
    assert(denied.retry_after_secs < 0.2);

    // Multi-token request: need 3 tokens, have ~0, rate=10/sec → ~0.3 sec
    auto denied2 = bucket.try_consume(3.0);
    assert(!denied2.allowed);
    assert(denied2.retry_after_secs > 0.2);
    assert(denied2.retry_after_secs < 0.5);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 7: Per-Client Isolation ────────────────────────────────────────────
// Different clients should have completely independent rate limits.
void test_per_client_isolation() {
    cout << "[TEST 7] Per-client bucket isolation..." << endl;
    RateLimiter limiter(2.0, 1.0);  // 2 max, 1/sec per client

    // Client A: consume both tokens
    assert(limiter.allow("client_A").allowed);
    assert(limiter.allow("client_A").allowed);
    assert(!limiter.allow("client_A").allowed);  // A is throttled

    // Client B: fresh bucket, should be allowed
    assert(limiter.allow("client_B").allowed);  // B is independent!
    assert(limiter.allow("client_B").allowed);
    assert(!limiter.allow("client_B").allowed);  // B now throttled too

    // Client C: still fresh
    assert(limiter.allow("client_C").allowed);

    assert(limiter.client_count() == 3);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 8: Client Removal ─────────────────────────────────────────────────
void test_client_removal() {
    cout << "[TEST 8] Client bucket removal..." << endl;
    RateLimiter limiter(3.0, 1.0);

    limiter.allow("192.168.1.1");
    limiter.allow("192.168.1.2");
    assert(limiter.client_count() == 2);

    assert(limiter.remove_client("192.168.1.1") == true);
    assert(limiter.client_count() == 1);

    // Removing non-existent client returns false
    assert(limiter.remove_client("10.0.0.1") == false);

    // Cleared client gets a fresh bucket on next request
    assert(limiter.allow("192.168.1.1").allowed);
    assert(limiter.client_count() == 2);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 9: Limiter-Wide Clear ──────────────────────────────────────────────
void test_clear_all() {
    cout << "[TEST 9] Clear all client buckets..." << endl;
    RateLimiter limiter(5.0, 1.0);

    for (int i = 0; i < 100; ++i) {
        limiter.allow("client_" + to_string(i));
    }
    assert(limiter.client_count() == 100);

    limiter.clear();
    assert(limiter.client_count() == 0);

    cout << "  -> Passed!" << endl;
}

// ─── TEST 10: Partial Refill Verification ────────────────────────────────────
// Verify that tokens are refilled proportionally to elapsed time, not all-or-nothing.
void test_partial_refill() {
    cout << "[TEST 10] Partial token refill (proportional to time)..." << endl;
    // Capacity 1,000 tokens, 10,000 tokens/sec refill rate.
    // Even if Windows timer resolution sleeps for 15ms, 15ms * 10,000 = 150 tokens,
    // which proves partial refill (150 < 1000 capacity).
    TokenBucket bucket(1000.0, 10000.0);

    // Drain completely
    assert(bucket.try_consume(1000.0).allowed);
    assert(!bucket.try_consume(1.0).allowed);

    // Sleep for ~5ms
    this_thread::sleep_for(chrono::milliseconds(5));

    // Consume 1 token at a time to count how many refilled
    int allowed_count = 0;
    while (bucket.try_consume(1.0).allowed) {
        ++allowed_count;
        if (allowed_count > 1000) break;
    }

    // Must have refilled some tokens, but definitely NOT full capacity (1000)
    assert(allowed_count >= 1);
    assert(allowed_count < 800);

    cout << "  -> Passed! (Refilled ~" << allowed_count << " tokens partially)" << endl;
}

// ─── TEST 11: Stress — Rapid Requests from Many Clients ─────────────────────
void test_stress_many_clients() {
    cout << "[TEST 11] Stress test: 1,000 clients × 10 requests each..." << endl;
    RateLimiter limiter(5.0, 1.0);  // 5 max, 1/sec

    int total_allowed = 0;
    int total_denied  = 0;

    for (int client = 0; client < 1000; ++client) {
        string id = "client_" + to_string(client);
        for (int req = 0; req < 10; ++req) {
            auto r = limiter.allow(id);
            if (r.allowed) ++total_allowed;
            else           ++total_denied;
        }
    }

    assert(limiter.client_count() == 1000);
    // Each client gets 5 allowed + 5 denied = 5000 allowed, 5000 denied
    assert(total_allowed == 5000);
    assert(total_denied  == 5000);

    cout << "  -> Passed! (" << total_allowed << " allowed, "
         << total_denied << " denied)" << endl;
}

// ─── TEST 12: Configuration Getters ──────────────────────────────────────────
void test_config_getters() {
    cout << "[TEST 12] Configuration getter verification..." << endl;
    TokenBucket bucket(42.0, 7.5);
    assert(bucket.max_tokens()  == 42.0);
    assert(bucket.refill_rate() == 7.5);

    RateLimiter limiter(100.0, 50.0);
    assert(limiter.max_tokens()  == 100.0);
    assert(limiter.refill_rate() == 50.0);

    cout << "  -> Passed!" << endl;
}

// ─── MAIN ────────────────────────────────────────────────────────────────────
int main() {
    cout << "========================================================" << endl;
    cout << "   GATEPULSE FR2: Token Bucket Rate Limiter — Tests     " << endl;
    cout << "========================================================" << endl;
    cout << endl;

    test_basic_allow_deny();
    test_refill_over_time();
    test_no_overflow();
    test_burst_then_throttle();
    test_multi_token_consume();
    test_retry_after_calculation();
    test_per_client_isolation();
    test_client_removal();
    test_clear_all();
    test_partial_refill();
    test_stress_many_clients();
    test_config_getters();

    cout << endl;
    cout << "========================================================" << endl;
    cout << "   ALL 12 TESTS PASSED — FR2 VERIFIED!                  " << endl;
    cout << "========================================================" << endl;

    return 0;
}
