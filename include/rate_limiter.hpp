// =============================================================================
// GatePulse — High-Performance L7 API Gateway & Reverse Proxy
// File: include/rate_limiter.hpp
// Milestone: FR2 — Token Bucket Rate Limiting
// =============================================================================
//
// Implements a per-client Token Bucket rate limiter with lazy refill.
//
// How it works:
//   1. Each client gets a bucket of `max_tokens` capacity (burst size).
//   2. Every request consumes 1 (or more) tokens from the bucket.
//   3. Tokens are NOT refilled by a background timer. Instead, on every
//      request we compute: new_tokens = elapsed_seconds * refill_rate,
//      and add them to the bucket (capped at max_tokens). This is the
//      "lazy refill" technique — zero threads, zero timers, O(1) per check.
//   4. If the bucket has insufficient tokens, the request is rejected and
//      a `retry_after_secs` value is calculated for the Retry-After header.
//
// Thread safety: NOT thread-safe in this milestone. Thread safety will be
// composed via the middleware pipeline (FR4) or can be added with a mutex.
// =============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <algorithm>

namespace gatepulse {

// ─── Rate Limit Result ───────────────────────────────────────────────────────
// Returned on every rate limit check. Contains all information needed to
// populate HTTP response headers:
//   - X-RateLimit-Remaining: tokens_remaining
//   - Retry-After:           retry_after_secs (only if denied)
struct RateLimitResult {
    bool   allowed;            // true = request may proceed
    double tokens_remaining;   // tokens left after this request (for headers)
    double retry_after_secs;   // seconds until a token is available (0 if allowed)
};

// ─── Token Bucket ────────────────────────────────────────────────────────────
// A single client's rate limit state. The bucket starts full (max_tokens) and
// drains as requests arrive. Tokens refill lazily on each check.
class TokenBucket {
public:
    TokenBucket(double max_tokens, double refill_rate_per_sec)
        : max_tokens_(max_tokens)
        , refill_rate_(refill_rate_per_sec)
        , current_tokens_(max_tokens)
        , last_refill_(std::chrono::steady_clock::now())
    {}

    // Attempt to consume `tokens` from the bucket.
    // Returns allowed=true if sufficient tokens exist, false otherwise.
    RateLimitResult try_consume(double tokens = 1.0) {
        refill();

        if (current_tokens_ >= tokens) {
            current_tokens_ -= tokens;
            return { true, current_tokens_, 0.0 };
        }

        // Calculate retry-after: how many seconds until `tokens` are available
        double deficit = tokens - current_tokens_;
        double retry_secs = deficit / refill_rate_;
        return { false, current_tokens_, retry_secs };
    }

    // Snapshot of available tokens WITHOUT refilling.
    // This is const-correct: observing state should not mutate it.
    double available_tokens() const {
        return current_tokens_;
    }

    // Getters for configuration (useful for testing and logging)
    double max_tokens()  const { return max_tokens_;  }
    double refill_rate() const { return refill_rate_; }

private:
    double max_tokens_;       // Bucket capacity (burst size)
    double refill_rate_;      // Tokens added per second
    double current_tokens_;   // Current token count
    std::chrono::steady_clock::time_point last_refill_;

    // ── Lazy Refill ──────────────────────────────────────────────────────
    // Computes elapsed time since the last check, multiplies by rate,
    // and adds tokens (capped at max_tokens). O(1) — no loops, no timers.
    //
    // Mathematical model:
    //   tokens(t) = min(max_tokens, tokens(t_prev) + (t - t_prev) * rate)
    //
    // This is a discrete approximation of the continuous ODE:
    //   dT/dt = rate,  T(0) = max_tokens,  T ∈ [0, max_tokens]
    void refill() {
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - last_refill_;

        double new_tokens = elapsed.count() * refill_rate_;
        current_tokens_ = std::min(max_tokens_, current_tokens_ + new_tokens);
        last_refill_ = now;
    }
};

// ─── Per-Client Rate Limiter ─────────────────────────────────────────────────
// Manages a collection of TokenBuckets keyed by client identifier (e.g., IP
// address, API key, user ID). Creates a new bucket on the first request from
// each client.
class RateLimiter {
public:
    RateLimiter(double max_tokens, double refill_rate_per_sec)
        : max_tokens_(max_tokens)
        , refill_rate_(refill_rate_per_sec)
    {}

    // Check if a client's request is allowed. Lazily creates a bucket
    // for first-time clients. Returns a RateLimitResult with remaining
    // tokens and retry-after timing.
    RateLimitResult allow(const std::string& client_id, double tokens = 1.0) {
        auto it = buckets_.find(client_id);
        if (it == buckets_.end()) {
            // First request from this client — create a full bucket
            auto [inserted, success] = buckets_.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(client_id),
                std::forward_as_tuple(max_tokens_, refill_rate_)
            );
            it = inserted;
        }
        return it->second.try_consume(tokens);
    }

    // Number of currently tracked clients.
    size_t client_count() const { return buckets_.size(); }

    // Remove a specific client's bucket (e.g., on disconnect or TTL expiry).
    bool remove_client(const std::string& client_id) {
        return buckets_.erase(client_id) > 0;
    }

    // Clear all client buckets (e.g., on configuration reload).
    void clear() { buckets_.clear(); }

    // Configuration getters
    double max_tokens()  const { return max_tokens_;  }
    double refill_rate() const { return refill_rate_; }

private:
    double max_tokens_;
    double refill_rate_;
    std::unordered_map<std::string, TokenBucket> buckets_;
};

}  // namespace gatepulse
