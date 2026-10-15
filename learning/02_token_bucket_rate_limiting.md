# Learning Note 02: Token Bucket Rate Limiting & std::chrono Internals

This document explains the deep "WHYs" behind every design and algorithmic decision made in Milestone 2.

---

## 1. WHY use Token Bucket over other rate limiting algorithms?

### The Question:
There are at least 5 common rate limiting algorithms. Why did GatePulse choose Token Bucket?

### The Deep Answer — All 5 Algorithms Compared:

#### Algorithm 1: Fixed Window Counter
```
Window: [00:00 - 01:00]  →  count = 0/100
Request arrives at 00:30  →  count = 1/100  ✓ Allow
...
Request arrives at 00:59  →  count = 100/100  ✗ Deny
Window resets at 01:00     →  count = 0/100  (fresh)
```

**Problem — The Boundary Burst:**
A client sends 100 requests at 00:59 (end of window 1) and 100 more at 01:01 (start of window 2). From the algorithm's perspective, both windows are within limits. But the server experienced **200 requests in 2 seconds** — double the intended rate.

$$\text{Effective burst} = 2 \times \text{limit} = 200 \text{ requests in } 2\text{s}$$

#### Algorithm 2: Sliding Window Log
Stores a timestamp for every single request. To check the rate, count timestamps within the last N seconds.

**Problem:** Memory. If the limit is 10,000 requests/minute and you have 50,000 clients, you're storing up to $10{,}000 \times 50{,}000 = 500{,}000{,}000$ timestamps. Each timestamp is 8 bytes → **4 GB of RAM** just for rate limiting.

#### Algorithm 3: Sliding Window Counter
A hybrid: combines the current window's count with a weighted fraction of the previous window's count.

$$\text{estimated\_count} = \text{prev\_count} \times (1 - \text{elapsed\_fraction}) + \text{curr\_count}$$

**Better than Fixed Window** (smooths the boundary), but still only provides an approximation. It cannot model burst behavior.

#### Algorithm 4: Leaky Bucket
Requests enter a FIFO queue (the "bucket"). The bucket drains at a constant rate. If the queue is full, requests are dropped.

**Problem:** Leaky Bucket enforces a **strictly constant output rate**. It cannot allow any burst at all. If your API can handle 10 concurrent requests, Leaky Bucket still forces 1-by-1 processing at the drain rate.

#### Algorithm 5: Token Bucket (GatePulse's choice) ✅
Tokens accumulate up to a cap. Each request consumes tokens. No tokens → deny.

**Why it wins for an API gateway:**

| Property | Fixed Window | Sliding Log | Sliding Counter | Leaky Bucket | **Token Bucket** |
| :--- | :---: | :---: | :---: | :---: | :---: |
| Allows controlled bursts | ❌ | ❌ | ❌ | ❌ | ✅ |
| Memory efficient | ✅ | ❌ | ✅ | ✅ | ✅ |
| No boundary edge case | ❌ | ✅ | ~✅ | ✅ | ✅ |
| Sustained rate + burst | ❌ | ❌ | ❌ | ❌ | ✅ |
| O(1) per check | ✅ | ❌ $O(N)$ | ✅ | ✅ | ✅ |

**The killer feature:** Token Bucket separates **burst capacity** (max_tokens) from **sustained rate** (refill_rate). An API gateway needs both:
- **Burst:** A client loading a dashboard makes 20 API calls simultaneously on page load. These should all succeed.
- **Sustained rate:** That same client shouldn't make 20 calls *every second* for an hour. The refill rate limits long-term throughput.

**Real-world usage:** AWS API Gateway, Google Cloud Endpoints, Stripe, and GitHub API all use Token Bucket or a close variant.

---

## 2. WHY lazy refill instead of a background timer thread?

### The Question:
Why not spawn a thread that runs `tokens += rate` every second for every client?

### The Deep Answer:

**Timer Thread Approach:**
```cpp
// ❌ BAD: Background refill thread
void refill_thread() {
    while (running_) {
        std::this_thread::sleep_for(1s);
        for (auto& [id, bucket] : buckets_) {
            bucket.tokens = std::min(bucket.max, bucket.tokens + bucket.rate);
        }
    }
}
```

**Problems with this approach:**

1. **Thread overhead:** For 100,000 connected clients, iterating all buckets every second burns CPU even if 99% of clients are idle. The timer thread does $O(N)$ work per tick regardless of actual request volume.

2. **Synchronization cost:** The refill thread writes to `tokens` while request-handling threads read it. This requires a mutex per bucket (or a global mutex). Under high load, lock contention becomes the bottleneck — not the actual request processing.

3. **Granularity trade-off:** If the timer fires every 1 second, a client that was denied at $t=0.01\text{s}$ must wait almost a full second even if they only needed $0.1\text{s}$ of refill. Finer granularity (e.g., 1ms ticks) means the thread wakes up 1,000 times/second — wasting CPU on context switches.

**Lazy Refill Approach (GatePulse's design):**
```cpp
// ✅ GOOD: Lazy refill — O(1), zero threads
void refill() {
    auto now = steady_clock::now();
    double elapsed = (now - last_refill_).count();
    current_tokens_ = min(max_tokens_, current_tokens_ + elapsed * refill_rate_);
    last_refill_ = now;
}
```

**Why this is superior:**
1. **Zero threads, zero timers.** The computation happens inline during the request check.
2. **O(1) per check** — one subtraction, one multiplication, one `min()`. ~3 CPU instructions.
3. **Perfect granularity** — the refill is computed to the nanosecond (via `steady_clock`). No discretization artifacts.
4. **Idle clients cost nothing** — if a client doesn't send requests, their bucket is never touched. This scales to millions of tracked clients with zero idle overhead.

### The Mathematical Model:
Token bucket state evolves as a piecewise-linear function:

$$T(t) = \min\!\Big(\text{max},\; T(t_{\text{prev}}) + (t - t_{\text{prev}}) \times \text{rate}\Big)$$

This is the analytic solution to the clamped ODE: $\frac{dT}{dt} = \text{rate}$, with $T \in [0, \text{max}]$.

Lazy refill evaluates this formula exactly at the moment of each request — no numerical integration error, no timer jitter.

---

## 3. WHY `std::chrono::steady_clock` instead of `system_clock`?

### The Question:
C++ provides three clocks: `system_clock`, `steady_clock`, `high_resolution_clock`. Why does GatePulse use `steady_clock`?

### The Deep Answer:

| Clock | Monotonic? | Adjustable? | Use Case |
| :--- | :---: | :---: | :--- |
| `system_clock` | ❌ | ✅ (NTP, user) | Wall-clock time, timestamps, logging |
| `steady_clock` | ✅ | ❌ | Measuring elapsed durations |
| `high_resolution_clock` | Platform-dependent | Platform-dependent | Typically an alias for one of the above |

**Why `system_clock` is dangerous for rate limiting:**

`system_clock` represents wall-clock time. It can be adjusted by:
1. **NTP synchronization** — the OS periodically syncs with time servers, which can jump the clock forward or backward by seconds.
2. **Daylight Saving Time** — the clock jumps by 1 hour.
3. **Manual adjustment** — a sysadmin sets the clock.

If the clock jumps **backward** by 5 seconds during a rate limit check:
```cpp
elapsed = now - last_refill_;  // Negative! e.g., -5 seconds
new_tokens = elapsed * rate;   // Negative tokens!
```
This would *subtract* tokens from the bucket, potentially making it permanently empty. The client gets permanently rate-limited until the server restarts.

If the clock jumps **forward** by 1 hour:
```cpp
elapsed = 3600 seconds
new_tokens = 3600 * 100 = 360,000 tokens  // Way beyond max
```
The `min(max, ...)` cap saves us here, but the timestamp is still wrong for subsequent calculations.

**`steady_clock` guarantees:**
- Time **never goes backward** (monotonically non-decreasing).
- The interval between ticks is constant (not affected by NTP or DST).
- It measures **physical elapsed time**, which is exactly what rate limiting needs.

### Interview Connection — The std::chrono Type System:
The `std::chrono` library uses a **type-safe duration system** that prevents unit confusion at compile time:

```cpp
using namespace std::chrono;

seconds      s = 5s;           // 5 seconds
milliseconds m = 5000ms;       // 5000 milliseconds
auto total = s + m;            // Compiles! Result is in milliseconds (5000 + 5000 = 10000ms)

// Duration cast for lossy conversions:
seconds rounded = duration_cast<seconds>(m);  // 5000ms → 5s (truncation, explicit)
```

The NASA Mars Climate Orbiter crashed because one team used pounds-force and another used newtons. C++ `chrono` prevents this class of bug at compile time — you cannot accidentally mix seconds and milliseconds without an explicit cast.

---

## 4. WHY use `double` for token counts instead of `int`?

### The Question:
Tokens are conceptually whole units. Why store them as `double`?

### The Deep Answer:

With integer tokens and a refill rate of 1 token/second, consider what happens between seconds:

```
t = 0.0s:  tokens = 10  (full)
t = 0.0s:  consume 10 → tokens = 0
t = 0.3s:  refill = 0.3 * 1 = 0.3 → truncated to 0 (integer!)  → DENIED
t = 0.5s:  refill = 0.5 * 1 = 0.5 → truncated to 0 (integer!)  → DENIED
t = 0.9s:  refill = 0.9 * 1 = 0.9 → truncated to 0 (integer!)  → DENIED
t = 1.0s:  refill = 1.0 * 1 = 1.0 → rounds to 1               → ALLOWED
```

With integers, the client experiences a **full 1-second blackout** after exhausting tokens, even though the effective rate should allow sub-second recovery.

With `double`:
```
t = 0.3s:  tokens = 0.3 (can't consume 1.0 → denied, but retry_after = 0.7s)
t = 1.0s:  tokens = 1.0 → allowed
```

The `double` representation also enables **fractional token costs** for differentiated rate limiting:
- `GET /api/users` costs 1.0 token (cheap read)
- `POST /api/upload` costs 5.0 tokens (expensive write)
- `GET /api/export` costs 10.0 tokens (very expensive bulk operation)

This is how Stripe and Shopify implement weighted rate limiting in production.

---

## 5. WHY return `RateLimitResult` instead of a simple `bool`?

### The Question:
`allow()` could just return `true`/`false`. Why return a struct with three fields?

### The Deep Answer:

HTTP rate limiting requires populating specific response headers (RFC 6585, IETF draft-ietf-httpapi-ratelimit-headers):

```http
HTTP/1.1 429 Too Many Requests
X-RateLimit-Limit: 100
X-RateLimit-Remaining: 0
Retry-After: 0.5
```

If `allow()` returned only a `bool`, the gateway would need to:
1. Call `allow()` → get `false`.
2. Call a separate `get_remaining()` → another function call, another lookup.
3. Call `get_retry_after()` → yet another computation.

This is both slower (3 calls vs 1) and error-prone (the state could change between calls in a concurrent system).

**GatePulse's `RateLimitResult` packs everything into one atomic return:**
```cpp
struct RateLimitResult {
    bool   allowed;            // → HTTP 200 vs 429
    double tokens_remaining;   // → X-RateLimit-Remaining header
    double retry_after_secs;   // → Retry-After header
};
```

One function call, one result, all data consistent. This is the **Command-Query Separation (CQS)** principle — the `try_consume()` command both mutates state (consuming tokens) and returns the query result (remaining tokens) in one transaction.

---

## 6. HOW does GatePulse's rate limiter compare to production systems?

### Real-World Implementations:

| System | Algorithm | Storage | Key Feature |
| :--- | :--- | :--- | :--- |
| **Nginx** (`limit_req`) | Leaky Bucket | Shared memory zone | `burst` parameter adds a queue before dropping |
| **Redis** (`redis-cell`) | Generic Cell Rate Algorithm (GCRA) | Redis key-value | Distributed rate limiting across multiple servers |
| **AWS API Gateway** | Token Bucket | Internal service | Per-API, per-stage, per-method limits |
| **Stripe API** | Token Bucket | Redis | Per-API-key with different tiers |
| **Cloudflare** | Sliding Window + Token Bucket | Edge workers | Distributed across 300+ PoPs |
| **GatePulse** | Token Bucket (lazy refill) | In-process `unordered_map` | Zero-thread, $O(1)$, nanosecond precision |

### Key Limitation of GatePulse's In-Process Design:
GatePulse stores buckets in a local `unordered_map`. If the gateway crashes, all rate limit state is lost — clients get fresh buckets on restart. In production, this state is typically stored in Redis or a shared memory segment (like Nginx's `limit_req_zone`).

### Interview Insight — Distributed Rate Limiting:
If an interviewer asks "how would you rate-limit across multiple gateway instances?", the answer is:
1. **Centralized store (Redis):** All gateway instances check/update a shared token bucket in Redis using atomic Lua scripts. This is what Stripe and GitHub do.
2. **Approximate local + sync:** Each instance maintains a local bucket and periodically syncs with a central store. Trades precision for lower latency.
3. **Sticky sessions:** Route each client to the same gateway instance (e.g., via consistent hashing — FR3!), so the local bucket is sufficient.
