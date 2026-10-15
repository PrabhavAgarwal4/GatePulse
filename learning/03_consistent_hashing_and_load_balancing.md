# Learning Note 03: Consistent Hashing, Virtual Nodes, & Upstream Health Management

This document explains the deep "WHYs" behind every design and algorithmic decision made in Milestone 3 of **GatePulse**.

---

## 1. WHY Consistent Hashing instead of Naive Modulo Hashing (`hash(key) % N`)?

### The Question:
When routing requests or partitioning cache data across $N$ servers, the most intuitive approach is:
$$\text{server\_index} = \text{hash}(\text{key}) \pmod N$$
Why is this approach catastrophic for production API gateways and distributed caches?

### The Deep Answer — The Cache Invalidation Storm:

Consider an API gateway distributing stateful requests or caching responses across $N = 4$ backend servers.

Suppose we have 12 keys with hash values: $0, 1, 2, \dots, 11$.
Under $N = 4$:
$$\text{key } k \implies k \pmod 4$$
- Server 0: Keys 0, 4, 8
- Server 1: Keys 1, 5, 9
- Server 2: Keys 2, 6, 10
- Server 3: Keys 3, 7, 11

Now, one server dies (or a new server is added to scale capacity) $\implies N = 3$.
Recalculating $k \pmod 3$:
- Key 0: $0 \pmod 3 = 0$ (Stayed on Server 0)
- Key 1: $1 \pmod 3 = 1$ (Stayed on Server 1)
- Key 2: $2 \pmod 3 = 2$ (Stayed on Server 2)
- Key 3: $3 \pmod 3 = 0$ (**Moved from 3 to 0!**)
- Key 4: $4 \pmod 3 = 1$ (**Moved from 0 to 1!**)
- Key 5: $5 \pmod 3 = 2$ (**Moved from 1 to 2!**)
- Key 6: $6 \pmod 3 = 0$ (**Moved from 2 to 0!**)
- Key 7: $7 \pmod 3 = 1$ (**Moved from 3 to 1!**)
- Key 8: $8 \pmod 3 = 2$ (**Moved from 0 to 2!**)
- Key 9: $9 \pmod 3 = 0$ (**Moved from 1 to 0!**)
- Key 10: $10 \pmod 3 = 1$ (**Moved from 2 to 1!**)
- Key 11: $11 \pmod 3 = 2$ (**Moved from 3 to 2!**)

Notice what just happened: **9 out of 12 keys (75%) changed their server destination!**
In general, under naive modulo hashing, adding or removing a node re-hashes:
$$\text{Disrupted Keys} = \frac{N-1}{N} \approx 75\% \text{ to } 99\%$$

### Consequence in Production:
1. **Cache Stampede / Thundering Herd:** Suddenly, $75\%-90\%$ of cached sessions/tokens are routed to the wrong server, missing the cache.
2. The backends get slammed with database queries simultaneously to rebuild the cache.
3. The database CPU spikes to 100%, causing cascading failure across the entire infrastructure.

---

## 2. HOW does Consistent Hashing solve this?

Consistent Hashing maps both **servers** and **request keys** onto the exact same circular 32-bit integer space:
$$\text{Ring} = [0, 2^{32} - 1]$$

```
                      0 / 2^32
                     /        \
                    /          \
            Server C            Server A
              (hash)            (hash)
                  \              /
                   \            /
                      Server B
                       (hash)
```

1. **Server Placement:** Each server is hashed by its identifier (e.g. `hash("server-A")`) to a point on the ring.
2. **Key Placement:** A client request key (e.g. `hash("client_ip_192.168.1.5")`) is hashed to a point on the ring.
3. **Routing Rule:** Walk **clockwise** from the key's point until you encounter the first server. That server handles the request.
4. **Minimal Disruption Property:**
   When a server is added or removed, **only the keys between it and its predecessor on the ring are remapped**.
   $$\text{Keys Disrupted} \approx \frac{1}{N}$$
   When going from 3 servers to 4 servers, only $\approx 25\%$ of keys move, while **$75\%$ of keys stay undisturbed**!

---

## 3. WHY are Virtual Nodes (V-Nodes) necessary?

### The Problem: Non-Uniform Skew and Hotspots
If we only place physical servers onto the ring:
- With 3 physical servers, their hashes might land at points: `100,000`, `150,000`, and `3,800,000,000`.
- Server C would own almost $90\%$ of the ring circumference, while Server A and B only share $10\%$.
- One physical server gets crushed with traffic (hotspot), while others sit idle.

### The Solution: Virtual Nodes:
Instead of placing each physical server once, GatePulse creates $R$ virtual replicas (e.g., $R = 100 \text{ to } 150$) per server:
```cpp
for (size_t i = 0; i < R; ++i) {
    uint32_t point = hash("server-A#vn#" + std::to_string(i));
    ring[point] = "server-A";
}
```

### The Mathematics:
By the Law of Large Numbers, distributing $N \times R$ points pseudo-randomly over the $2^{32}$ interval reduces the standard deviation of load across servers:
$$\sigma \propto \frac{1}{\sqrt{R}}$$
With $R = 150$, load variance across all physical servers drops below $\pm 5\%$, achieving near-perfect balance.

---

## 4. WHY `std::map::lower_bound` instead of Linear Scan?

### The Question:
How do we find the first server clockwise from the key's hash point?

### The Naive Way:
Iterate through an array of all virtual nodes:
- Time: $O(V)$ where $V = N \times R$.
- For 100 servers with 150 virtual nodes: $15,000$ comparisons per incoming HTTP request!

### The Red-Black Tree Way (`std::map`):
`std::map<uint32_t, string>` keeps all virtual ring points in a Self-Balancing Binary Search Tree (Red-Black Tree).
```cpp
auto it = ring_.lower_bound(key_hash);
if (it == ring_.end()) {
    it = ring_.begin(); // Wrap-around
}
```
- `lower_bound(key_hash)` performs binary search on the tree in $O(\log V)$ time.
- For $V = 15,000$ points, $\log_2(15,000) \approx 14$ comparisons.
- **Result:** Route resolution completes in **less than 1 microsecond**.

---

## 5. HOW does Failover and Health Tracking Work?

In a high-availability gateway, backends can crash or fail health checks at any moment.

```cpp
auto it = ring_.lower_bound(key_hash);
do {
    const std::string& server_id = it->second;
    if (servers_[server_id].healthy) {
        return servers_[server_id]; // Found active healthy node!
    }
    ++it; // Walk clockwise to next virtual node
    if (it == ring_.end()) it = ring_.begin();
} while (it != start_it);
```

1. **Clockwise Fallback:** If the primary hashed virtual node belongs to an unhealthy server, the pointer simply steps clockwise (`++it`) to the next virtual node.
2. **Determinism:** All requests for that key automatically fall over to the exact same secondary server without any explicit state synchronization.
3. **Recovery:** When the primary server recovers its health check, requests naturally resume routing to it without ring reconfiguration.
4. **All Unhealthy:** If all servers are marked unhealthy, GatePulse returns `std::nullopt`, allowing the gateway to respond with `HTTP 503 Service Unavailable`.

---

## 6. Real-World Systems Comparison

| System | Load Balancing Mechanism | Virtual Node Count | Failover Strategy |
| :--- | :--- | :--- | :--- |
| **Nginx** | `chash` (Consistent Hashing) | 160 per server | Next slot in hash table |
| **Envoy Proxy** | Maglev / Ring Hash | Configurable (default 1024) | Active health checking + fallback |
| **HAProxy** | `balance url_param` / `chash` | Configurable | Dynamic server weight adjustment |
| **Discord** | Consistent Hash Ring | 256 v-nodes | Erlang ring distributed state |
| **Cassandra / DynamoDB** | Partition Token Ring | 256 v-nodes (vnodes) | Quorum replication across neighbors |
| **GatePulse** | 32-bit Murmur-FNV-1a Ring | 100-150 v-nodes (weighted) | Clockwise healthy traversal ($O(\log V)$) |
