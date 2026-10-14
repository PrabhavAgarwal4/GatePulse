// =============================================================================
// GatePulse — High-Performance L7 API Gateway & Reverse Proxy
// File: include/trie_router.hpp
// Milestone: FR1 — Trie Route Matching Engine
// =============================================================================
//
// A segment-based Trie for O(k) URL route resolution, where k is the number
// of path segments (NOT characters). Supports:
//   - Static routes:       /api/health
//   - Parameterized routes: /api/users/:id/profile
//   - Wildcard catch-all:  /static/*
//
// Match priority per node: Static > Parameterized > Wildcard
// This guarantees that /users/admin always beats /users/:id when both exist.
// =============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <memory>
#include <sstream>

namespace gatepulse {

// ─── HTTP Method Enum ────────────────────────────────────────────────────────
enum class HttpMethod {
    GET, POST, PUT, DELETE_METHOD, PATCH, HEAD, OPTIONS
};

inline std::string method_to_string(HttpMethod m) {
    switch (m) {
        case HttpMethod::GET:           return "GET";
        case HttpMethod::POST:          return "POST";
        case HttpMethod::PUT:           return "PUT";
        case HttpMethod::DELETE_METHOD: return "DELETE";
        case HttpMethod::PATCH:         return "PATCH";
        case HttpMethod::HEAD:          return "HEAD";
        case HttpMethod::OPTIONS:       return "OPTIONS";
        default:                        return "UNKNOWN";
    }
}

// ─── Route Match Result ──────────────────────────────────────────────────────
// Returned on a successful match. Contains the handler identifier and any
// extracted path parameters (e.g., :id → "42").
struct RouteMatch {
    std::string handler_name;
    std::unordered_map<std::string, std::string> params;
};

// ─── Trie Router ─────────────────────────────────────────────────────────────
class TrieRouter {
public:
    // Register a route pattern with a handler name for a specific HTTP method.
    // Pattern examples: "/api/users/:id", "/static/*", "/health"
    void add_route(HttpMethod method, const std::string& pattern,
                   const std::string& handler_name) {
        auto segments = split_path(pattern);
        TrieNode* current = &root_;

        for (size_t i = 0; i < segments.size(); ++i) {
            const auto& seg = segments[i];

            if (seg == "*") {
                // ── Wildcard: catches everything from this point onward ──
                if (!current->wildcard_child) {
                    current->wildcard_child = std::make_unique<TrieNode>();
                }
                current = current->wildcard_child.get();
                break;  // Nothing can come after a wildcard
            } else if (!seg.empty() && seg[0] == ':') {
                // ── Parameterized segment: e.g. ":id", ":version" ──
                std::string param_name = seg.substr(1);
                if (!current->param_child) {
                    current->param_child = std::make_unique<TrieNode>();
                    current->param_child->param_name = param_name;
                }
                current = current->param_child.get();
            } else {
                // ── Static segment: exact string match ──
                auto it = current->static_children.find(seg);
                if (it == current->static_children.end()) {
                    current->static_children[seg] = std::make_unique<TrieNode>();
                }
                current = current->static_children[seg].get();
            }
        }

        current->handlers[method] = handler_name;
        ++route_count_;
    }

    // Attempt to match a request path against all registered routes.
    // Returns std::nullopt if no matching route exists for the given method.
    std::optional<RouteMatch> match(HttpMethod method,
                                    const std::string& path) const {
        auto segments = split_path(path);
        RouteMatch result;

        if (match_recursive(&root_, segments, 0, method, result)) {
            return result;
        }
        return std::nullopt;
    }

    // Total number of registered (method, pattern) combinations.
    size_t route_count() const { return route_count_; }

private:
    // ─── Internal Trie Node ──────────────────────────────────────────────
    // Each node can hold:
    //   1. A map of static children    (exact segment → child)
    //   2. At most one param child     (matches any single segment)
    //   3. At most one wildcard child  (matches all remaining segments)
    //   4. Handler map per HTTP method (GET on this path ≠ POST on this path)
    struct TrieNode {
        std::unordered_map<std::string, std::unique_ptr<TrieNode>> static_children;

        std::unique_ptr<TrieNode> param_child;
        std::string param_name;   // e.g. "id" for pattern segment ":id"

        std::unique_ptr<TrieNode> wildcard_child;

        // A node may be a terminal for multiple HTTP methods simultaneously.
        // e.g., GET /api/users → "list_users", POST /api/users → "create_user"
        std::unordered_map<HttpMethod, std::string> handlers;
    };

    TrieNode root_;
    size_t route_count_ = 0;

    // ─── Path Splitting ──────────────────────────────────────────────────
    // "/api/users/:id" → ["api", "users", ":id"]
    // "/"              → []  (empty segments are discarded)
    static std::vector<std::string> split_path(const std::string& path) {
        std::vector<std::string> segments;
        std::istringstream stream(path);
        std::string segment;

        while (std::getline(stream, segment, '/')) {
            if (!segment.empty()) {
                segments.push_back(std::move(segment));
            }
        }
        return segments;
    }

    // ─── Recursive Matching with Backtracking ────────────────────────────
    // Tries matches in strict priority: Static → Param → Wildcard.
    // If a static branch fails deeper in the tree, we backtrack and try the
    // param branch, then the wildcard branch. This guarantees correct
    // priority without sacrificing completeness.
    bool match_recursive(const TrieNode* node,
                         const std::vector<std::string>& segments,
                         size_t index, HttpMethod method,
                         RouteMatch& result) const {
        // Base case: all segments consumed — check if this node has a handler
        if (index == segments.size()) {
            auto it = node->handlers.find(method);
            if (it != node->handlers.end()) {
                result.handler_name = it->second;
                return true;
            }
            return false;
        }

        const std::string& seg = segments[index];

        // ── Priority 1: Static child (exact segment match) ──
        auto static_it = node->static_children.find(seg);
        if (static_it != node->static_children.end()) {
            if (match_recursive(static_it->second.get(), segments,
                                index + 1, method, result)) {
                return true;
            }
        }

        // ── Priority 2: Parameterized child (captures one segment) ──
        if (node->param_child) {
            result.params[node->param_child->param_name] = seg;
            if (match_recursive(node->param_child.get(), segments,
                                index + 1, method, result)) {
                return true;
            }
            // Backtrack: remove the captured param if this branch fails
            result.params.erase(node->param_child->param_name);
        }

        // ── Priority 3: Wildcard child (captures all remaining segments) ──
        if (node->wildcard_child) {
            auto it = node->wildcard_child->handlers.find(method);
            if (it != node->wildcard_child->handlers.end()) {
                result.handler_name = it->second;
                // Join remaining segments into the "*" param
                std::string remaining;
                for (size_t i = index; i < segments.size(); ++i) {
                    if (i > index) remaining += '/';
                    remaining += segments[i];
                }
                result.params["*"] = remaining;
                return true;
            }
        }

        return false;
    }
};

}  // namespace gatepulse
