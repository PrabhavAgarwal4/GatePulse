// =============================================================================
// GatePulse — High-Performance L7 API Gateway & Reverse Proxy
// File: include/middleware.hpp
// Milestone: FR4 — Middleware Pipeline & Chain of Responsibility
// =============================================================================
//
// Implements an Onion-Architecture Middleware Pipeline (Chain of Responsibility).
//
// Key Concepts:
//   1. HttpContext: Unifies HttpRequest and HttpResponse with an extensible
//      attribute map for passing state between middlewares (e.g., user_id).
//   2. NextFunction: std::function<void()> representing the tail of the chain.
//   3. Onion Execution:
//        Middleware 1 (Pre) -> Middleware 2 (Pre) -> Handler
//        Middleware 1 (Post) <- Middleware 2 (Post) <-
//   4. Short-Circuiting: If a middleware does NOT call next(), execution stops
//      immediately (e.g., Auth failure -> 401 Unauthorized, RateLimit -> 429).
// =============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <sstream>
#include "rate_limiter.hpp"

namespace gatepulse {

// ─── HTTP Request Representation ─────────────────────────────────────────────
struct HttpRequest {
    std::string method;                                      // "GET", "POST", etc.
    std::string path;                                        // "/api/v1/users"
    std::unordered_map<std::string, std::string> headers;    // Header key-value pairs
    std::unordered_map<std::string, std::string> params;     // Path params (from Trie)
    std::string body;                                        // Request payload
    std::string client_ip;                                   // Remote client IP

    std::string get_header(const std::string& key) const {
        auto it = headers.find(key);
        if (it != headers.end()) return it->second;
        return "";
    }

    void set_header(const std::string& key, const std::string& val) {
        headers[key] = val;
    }
};

// ─── HTTP Response Representation ────────────────────────────────────────────
struct HttpResponse {
    int status_code = 200;                                   // 200, 401, 404, 429, etc.
    std::string status_message = "OK";                       // "OK", "Not Found", etc.
    std::unordered_map<std::string, std::string> headers;    // Response headers
    std::string body;                                        // Response payload

    void set_status(int code, const std::string& message) {
        status_code = code;
        status_message = message;
    }

    void set_header(const std::string& key, const std::string& val) {
        headers[key] = val;
    }

    std::string get_header(const std::string& key) const {
        auto it = headers.find(key);
        if (it != headers.end()) return it->second;
        return "";
    }
};

// ─── HTTP Context ────────────────────────────────────────────────────────────
// Carries the request and response across all pipeline filters, along with
// arbitrary attributes for cross-middleware communication.
struct HttpContext {
    HttpRequest  request;
    HttpResponse response;
    std::unordered_map<std::string, std::string> attributes; // Dynamic state (e.g., "auth_user")

    void set_attribute(const std::string& key, const std::string& value) {
        attributes[key] = value;
    }

    std::string get_attribute(const std::string& key) const {
        auto it = attributes.find(key);
        if (it != attributes.end()) return it->second;
        return "";
    }
};

// ─── Middleware Signatures ───────────────────────────────────────────────────
using NextFunction = std::function<void()>;
using Middleware   = std::function<void(HttpContext&, NextFunction)>;
using HttpHandler  = std::function<void(HttpContext&)>;

// ─── Middleware Pipeline ─────────────────────────────────────────────────────
class MiddlewarePipeline {
public:
    // Append a middleware to the end of the chain
    void use(Middleware mw) {
        middlewares_.push_back(std::move(mw));
    }

    // Execute the complete pipeline on an HttpContext ending at final_handler
    void execute(HttpContext& ctx, HttpHandler final_handler) const {
        dispatch(0, ctx, final_handler);
    }

    size_t size() const { return middlewares_.size(); }

private:
    std::vector<Middleware> middlewares_;

    // Recursive dispatcher enabling Continuation-Passing Style execution
    void dispatch(size_t index, HttpContext& ctx, const HttpHandler& final_handler) const {
        if (index < middlewares_.size()) {
            // Next continuation wraps the call to the next middleware
            NextFunction next = [this, index, &ctx, &final_handler]() {
                dispatch(index + 1, ctx, final_handler);
            };
            middlewares_[index](ctx, next);
        } else {
            // Reached the center of the onion -> execute the destination handler
            final_handler(ctx);
        }
    }
};

// ─── BUILT-IN PRODUCTION MIDDLEWARES ─────────────────────────────────────────

// 1. Logging & Latency Middleware
// Records total execution time and sets the X-Response-Time-Ms header
inline Middleware make_logging_middleware(std::vector<std::string>* log_sink = nullptr) {
    return [log_sink](HttpContext& ctx, NextFunction next) {
        auto start = std::chrono::steady_clock::now();

        next(); // Yield to downstream middlewares and handler

        auto end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        ctx.response.set_header("X-Response-Time-Ms", std::to_string(elapsed_ms));

        if (log_sink) {
            std::ostringstream ss;
            ss << ctx.request.method << " " << ctx.request.path << " -> "
               << ctx.response.status_code << " (" << elapsed_ms << " ms)";
            log_sink->push_back(ss.str());
        }
    };
}

// 2. Authentication Middleware
// Enforces that Authorization header exists and matches expected token
inline Middleware make_auth_middleware(const std::string& expected_token) {
    return [expected_token](HttpContext& ctx, NextFunction next) {
        std::string auth_header = ctx.request.get_header("Authorization");
        std::string prefix = "Bearer ";

        if (auth_header.rfind(prefix, 0) == 0) {
            std::string token = auth_header.substr(prefix.length());
            if (token == expected_token) {
                ctx.set_attribute("authenticated", "true");
                ctx.set_attribute("auth_token", token);
                next(); // Token valid -> proceed!
                return;
            }
        }

        // Token missing or invalid -> Short-circuit with 401 Unauthorized
        ctx.response.set_status(401, "Unauthorized");
        ctx.response.body = "{\"error\":\"Unauthorized: invalid or missing bearer token\"}";
        ctx.response.set_header("Content-Type", "application/json");
    };
}

// 3. CORS (Cross-Origin Resource Sharing) Middleware
inline Middleware make_cors_middleware(const std::string& allow_origin = "*",
                                       const std::string& allow_methods = "GET, POST, PUT, DELETE, OPTIONS",
                                       const std::string& allow_headers = "Content-Type, Authorization, X-Requested-With") {
    return [allow_origin, allow_methods, allow_headers](HttpContext& ctx, NextFunction next) {
        ctx.response.set_header("Access-Control-Allow-Origin", allow_origin);
        ctx.response.set_header("Access-Control-Allow-Methods", allow_methods);
        ctx.response.set_header("Access-Control-Allow-Headers", allow_headers);

        if (ctx.request.method == "OPTIONS") {
            // Pre-flight check -> short-circuit immediately with 204 No Content
            ctx.response.set_status(204, "No Content");
            return;
        }

        next(); // Normal request -> proceed to inner pipeline
    };
}

// 4. Rate Limiting Middleware (Integrated with Milestone 2 RateLimiter)
inline Middleware make_rate_limit_middleware(std::shared_ptr<RateLimiter> limiter) {
    return [limiter](HttpContext& ctx, NextFunction next) {
        std::string client_key = ctx.request.client_ip;
        if (client_key.empty()) {
            client_key = "anonymous";
        }

        auto result = limiter->allow(client_key);

        ctx.response.set_header("X-RateLimit-Remaining", std::to_string(result.tokens_remaining));

        if (!result.allowed) {
            // Short-circuit with 429 Too Many Requests
            ctx.response.set_status(429, "Too Many Requests");
            ctx.response.set_header("Retry-After", std::to_string(result.retry_after_secs));
            ctx.response.body = "{\"error\":\"Rate limit exceeded. Please retry later.\"}";
            ctx.response.set_header("Content-Type", "application/json");
            return;
        }

        next(); // Allowed -> proceed
    };
}

// 5. Security Headers Transformation Middleware
inline Middleware make_security_headers_middleware() {
    return [](HttpContext& ctx, NextFunction next) {
        next(); // Run inner handlers first

        // Inject production security headers on the way out
        ctx.response.set_header("X-Content-Type-Options", "nosniff");
        ctx.response.set_header("X-Frame-Options", "DENY");
        ctx.response.set_header("X-XSS-Protection", "1; mode=block");
        ctx.response.set_header("Server", "GatePulse/1.0");
    };
}

}  // namespace gatepulse
