// =============================================================================
// GatePulse — High-Performance L7 API Gateway & Reverse Proxy
// File: include/load_balancer.hpp
// Milestone: FR3 — Consistent Hash Load Balancing & Upstream Health Management
// =============================================================================
//
// Implements a Consistent Hash Ring on a 32-bit keyspace [0, 2^32 - 1]
// with Virtual Nodes (replicas) to eliminate non-uniform hash skew.
//
// Key Features:
//   1. Virtual Nodes: Maps physical upstreams to R points across the ring.
//   2. Minimal Disruption: Adding/removing a node redistributes ~1/N keys.
//   3. O(log V) Lookup: std::map binary search via lower_bound with wrap-around.
//   4. Health-Aware Routing: Automatically skips unhealthy/offline nodes by
//      walking clockwise until a healthy upstream is located.
//   5. Weighted Distribution: Configurable replicas per upstream.
// =============================================================================
#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cstdint>
#include <algorithm>
#include <sstream>

namespace gatepulse {

// ─── High-Dispersion 32-bit Hash Function (FNV-1a with Murmur Avalanche) ─────
// Standard std::hash produces sequential clustering for short strings.
// This function combines FNV-1a with a 32-bit finalizer mixing step to ensure
// uniform pseudo-random dispersion across the entire 2^32 ring.
inline uint32_t hash_ring_key(const std::string& key) {
    uint32_t hash = 2166136261u; // FNV-1a 32-bit offset basis
    for (char c : key) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;        // FNV prime
    }
    // MurmurHash3 32-bit integer finalizer avalanche
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    return hash;
}

// ─── Upstream Node Representation ────────────────────────────────────────────
struct UpstreamServer {
    std::string id;       // Unique server identifier (e.g. "srv-app-01")
    std::string host;     // IP address or hostname (e.g. "127.0.0.1")
    uint16_t    port;     // TCP listening port (e.g. 8081)
    bool        healthy;  // Active health status
    size_t      weight;   // Weight multiplier for virtual node replication

    std::string endpoint() const {
        return host + ":" + std::to_string(port);
    }
};

// ─── Consistent Hash Ring Load Balancer ───────────────────────────────────────
class ConsistentHashLoadBalancer {
public:
    // default_replicas: number of virtual nodes placed per weight unit
    explicit ConsistentHashLoadBalancer(size_t default_replicas = 100)
        : default_replicas_(default_replicas) {}

    // Register a new upstream server to the cluster.
    // Adds (default_replicas * weight) virtual points onto the ring.
    void add_server(const UpstreamServer& server) {
        servers_[server.id] = server;
        size_t total_vnodes = default_replicas_ * (server.weight > 0 ? server.weight : 1);

        for (size_t i = 0; i < total_vnodes; ++i) {
            std::string vnode_key = server.id + "#vn#" + std::to_string(i);
            uint32_t ring_point = hash_ring_key(vnode_key);
            ring_[ring_point] = server.id;
        }
    }

    // Remove an upstream server completely from the ring.
    bool remove_server(const std::string& server_id) {
        auto it = servers_.find(server_id);
        if (it == servers_.end()) {
            return false;
        }

        size_t total_vnodes = default_replicas_ * (it->second.weight > 0 ? it->second.weight : 1);
        for (size_t i = 0; i < total_vnodes; ++i) {
            std::string vnode_key = server_id + "#vn#" + std::to_string(i);
            uint32_t ring_point = hash_ring_key(vnode_key);
            ring_.erase(ring_point);
        }

        servers_.erase(it);
        return true;
    }

    // Update the health status of an upstream server.
    // If unhealthy, traffic will automatically skip this node on the ring.
    bool set_health(const std::string& server_id, bool healthy) {
        auto it = servers_.find(server_id);
        if (it == servers_.end()) {
            return false;
        }
        it->second.healthy = healthy;
        return true;
    }

    // Route a request key (e.g. client IP, session cookie, URL) to a healthy upstream.
    // Returns std::nullopt if the cluster is empty or all servers are unhealthy.
    std::optional<UpstreamServer> get_server(const std::string& routing_key) const {
        if (ring_.empty() || servers_.empty()) {
            return std::nullopt;
        }

        uint32_t key_hash = hash_ring_key(routing_key);

        // Find the first virtual node with ring_point >= key_hash (Clockwise search)
        auto it = ring_.lower_bound(key_hash);
        if (it == ring_.end()) {
            // Wrap-around to the start of the ring (0)
            it = ring_.begin();
        }

        auto start_it = it;
        std::unordered_set<std::string> inspected_nodes;

        // Traverse clockwise until a healthy physical node is found
        do {
            const std::string& server_id = it->second;
            auto srv_it = servers_.find(server_id);
            if (srv_it != servers_.end() && srv_it->second.healthy) {
                return srv_it->second;
            }

            inspected_nodes.insert(server_id);
            if (inspected_nodes.size() == servers_.size()) {
                // All registered physical servers are unhealthy
                return std::nullopt;
            }

            ++it;
            if (it == ring_.end()) {
                it = ring_.begin(); // Ring wrap-around
            }
        } while (it != start_it);

        return std::nullopt;
    }

    // Inspect server count & virtual node count
    size_t server_count() const { return servers_.size(); }
    size_t ring_points_count() const { return ring_.size(); }

    std::optional<UpstreamServer> get_server_metadata(const std::string& server_id) const {
        auto it = servers_.find(server_id);
        if (it != servers_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

private:
    size_t default_replicas_;
    std::unordered_map<std::string, UpstreamServer> servers_;
    // Red-Black Tree maintaining the 32-bit ring points in sorted ascending order
    std::map<uint32_t, std::string> ring_;
};

}  // namespace gatepulse
