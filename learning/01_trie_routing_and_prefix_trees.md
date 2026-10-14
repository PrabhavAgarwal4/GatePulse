# Learning Note 01: Trie Routing & Prefix Tree Internals

This document explains the deep "WHYs" behind every design and algorithmic decision made in Milestone 1.

---

## 1. WHY use a Trie instead of a HashMap for URL routing?

### The Question:
`std::unordered_map` gives us $O(1)$ lookup. Why not just store `{"/api/users/42": handler}` in a hash map?

### The Deep Answer:

A HashMap requires **exact key matching**. But API gateways need **pattern matching**:

```
Registered:  /api/users/:id
Request:     /api/users/42       ← must match and extract id=42
Request:     /api/users/alice    ← must also match, id=alice
```

To use a HashMap, you would need to enumerate every possible value of `:id` — which is infinite. There is no way to store `/api/users/:id` as a hash key and have it match `/api/users/42`.

**A Trie solves this naturally:**
- Each level of the Trie corresponds to one URL segment.
- A parameterized node (`:id`) acts as a wildcard for a single level — it matches *any* segment value and captures it.
- The lookup walks down the tree segment by segment, taking $O(k)$ time where $k$ is the number of segments (typically 3–6 for REST APIs).

| Approach | Static Routes | Parameterized Routes | Wildcard Routes | Complexity |
| :--- | :---: | :---: | :---: | :--- |
| HashMap | ✅ | ❌ | ❌ | $O(1)$ amortized |
| Regex List | ✅ | ✅ | ✅ | $O(N \cdot L)$ — scans ALL routes |
| **Trie** | ✅ | ✅ | ✅ | $O(k)$ — segments only |

**Real-world:** Express.js (Node), Gin (Go), and Actix (Rust) all use Trie-based routers internally.

---

## 2. WHY use a segment-based Trie instead of a character-based Trie?

### The Question:
Classic textbook Tries split strings into individual characters. Why does GatePulse split by `/` into segments instead?

### The Deep Answer:

Consider the URL `/api/users/42`:

**Character-based Trie (13 nodes):**
```
root → a → p → i → / → u → s → e → r → s → / → 4 → 2
```

**Segment-based Trie (3 nodes):**
```
root → "api" → "users" → "42"
```

1. **Space efficiency:** A character-based Trie for 1,000 routes with average path length ~30 characters creates ~30,000 nodes. A segment-based Trie with ~4 segments per path creates ~4,000 nodes. That's a **7.5× reduction** in memory.

2. **Parameterized matching:** With character-based tries, implementing `:id` requires complex prefix detection — you'd have to recognize the `:` character, then switch to "consume everything until the next `/`" mode. With segment-based tries, `:id` is simply a special child type at one Trie level. Clean and elegant.

3. **Why this doesn't hurt performance:** URL segments are naturally delimited by `/`. Splitting by `/` is a single linear scan — no overhead beyond what the HTTP parser already does.

### The Trade-Off — Radix Trees (Patricia Tries):
Nginx uses a **Radix Tree** (also called a compressed Trie or Patricia Trie) which is character-based but merges single-child chains into one node:

```
Before compression:  root → a → p → i
After compression:   root → "api"
```

This gives character-level flexibility (useful for Nginx's `location` prefix matching) with reduced memory. GatePulse uses segment-based tries because REST API routes are naturally segment-oriented — we don't need character-level prefix matching.

---

## 3. WHY does match priority matter (Static > Param > Wildcard)?

### The Question:
When both `/users/admin` (static) and `/users/:id` (parameterized) are registered, which one should win for the request `/users/admin`?

### The Deep Answer:

**Static MUST win.** Here's the real-world reason:

Imagine an API gateway with these routes:
```
GET /users/:id      → upstream user-service (fetch any user by ID)
GET /users/admin    → upstream admin-panel  (special admin dashboard)
```

If `:id` wins, then an admin visiting `/users/admin` gets routed to the user-service, which tries to look up a user with `id="admin"` and returns a 404. The admin panel becomes unreachable.

**The priority hierarchy is universally agreed upon across all major frameworks:**

| Priority | Type | Example | Rationale |
| :---: | :--- | :--- | :--- |
| 1 (highest) | Static | `/users/admin` | Most specific — exact string match |
| 2 | Parameterized | `/users/:id` | Semi-specific — matches any single segment |
| 3 (lowest) | Wildcard | `/users/*` | Least specific — matches everything |

### Implementation: Backtracking
GatePulse implements this priority using **recursive backtracking**:

```cpp
// Priority 1: Try static match
if (static_children.contains(segment)) {
    if (recurse into static child succeeds) return true;
}
// Priority 2: Try param match (only if static failed)
if (param_child exists) {
    capture param; if (recurse succeeds) return true; else erase param;
}
// Priority 3: Try wildcard (only if both above failed)
if (wildcard_child exists) { ... }
```

The key insight is the **backtracking on param**: if we capture `id="admin"` and the param branch doesn't lead to a valid handler deeper in the Trie, we *undo* the capture and try the next priority. This is the same algorithmic pattern as **DFS with backtracking** used in constraint satisfaction and puzzle solving.

---

## 4. WHY store handlers per HTTP method at each Trie node?

### The Question:
Why does each `TrieNode` store `unordered_map<HttpMethod, string> handlers` instead of a single handler?

### The Deep Answer:

In RESTful API design, the **same URL path** serves completely different purposes based on the HTTP method:

```
GET    /api/users  →  List all users     (read operation)
POST   /api/users  →  Create a new user  (write operation)
```

If a Trie node stored only one handler, registering `POST /api/users` would overwrite the `GET /api/users` handler. This would break REST semantics.

**Real-world significance:**
- This is why the HTTP specification (RFC 7231) defines methods as a primary dispatching mechanism.
- API gateways like Kong and Envoy route on `(method, path)` tuples, not just paths.
- When a path matches but the method doesn't, the gateway should return **405 Method Not Allowed** (not 404). GatePulse can detect this because the Trie node exists (path matched) but `handlers.find(method)` fails.

---

## 5. WHY use `std::unique_ptr` for Trie children?

### The Question:
Why not store `TrieNode` children by value or use raw pointers?

### The Deep Answer:

1. **Why not by value (`TrieNode child;`)?**
   - `TrieNode` contains `unordered_map<string, unique_ptr<TrieNode>>` — it is a **recursive type**. A struct cannot contain a member of its own type by value because the compiler cannot compute `sizeof(TrieNode)` (it would be infinite).
   - Pointers break the recursion: `sizeof(unique_ptr<TrieNode>)` is always 8 bytes (a single pointer), regardless of what `TrieNode` contains.

2. **Why not raw pointers (`TrieNode*`)?**
   - Raw pointers require manual `delete` in a destructor. If the Trie has 10 levels deep with branching, writing a correct recursive destructor is error-prone.
   - `std::unique_ptr` provides **RAII (Resource Acquisition Is Initialization)**: when a `TrieNode` is destroyed, all its `unique_ptr` children are automatically destroyed recursively. Zero memory leaks, zero manual cleanup.

3. **Why not `shared_ptr`?**
   - `std::shared_ptr` maintains a reference count (atomic increment/decrement on every copy). This adds ~50 bytes overhead per node and unnecessary atomic operations.
   - Trie children have **single ownership** — a parent node exclusively owns its children. `unique_ptr` perfectly models this with zero overhead beyond a raw pointer.

### Interview Connection — RAII:
RAII is one of C++'s most important idioms. The principle: tie the lifetime of a resource (memory, file handle, socket, lock) to the lifetime of an object. When the object's destructor runs, the resource is released. This eliminates entire categories of bugs:
- Memory leaks (unique_ptr)
- Deadlocks (lock_guard)
- File descriptor leaks (fstream)
- Socket leaks (RAII socket wrappers, which we'll build in FR5)

---

## 6. WHAT is the time and space complexity of Trie routing?

### Time Complexity:

| Operation | Complexity | Explanation |
| :--- | :--- | :--- |
| `add_route()` | $O(k)$ | Walk $k$ segments, create nodes as needed |
| `match()` (best) | $O(k)$ | All segments match static children — single path |
| `match()` (worst) | $O(k \cdot 2^k)$ | Pathological: every level has both static and param children, requiring backtracking at each level |

**Why the worst case is unrealistic:**
In practice, routes don't have both a static and parameterized child at every single level. A typical REST API has at most 1–2 levels where static and param compete (e.g., `/users/admin` vs `/users/:id`). So real-world matching is effectively $O(k)$ where $k \le 6$.

### Space Complexity:

- **Per Node:** ~120 bytes (unordered_map overhead + two unique_ptrs + string + handler map)
- **For $R$ routes with average $k$ segments:** $O(R \cdot k)$ nodes in the worst case (no shared prefixes). In practice, API routes share prefixes heavily (`/api/v1/...`), so actual node count is much lower.
- **Comparison to Regex list:** A regex list stores $R$ compiled regex objects. Our Trie stores shared prefix structure. For 100 routes sharing `/api/v1/`, the Trie stores those 3 prefix nodes once; a regex list stores them 100 times.

---

## 7. HOW do real API gateways handle routing at scale?

### Production Routing Strategies:

| Gateway | Routing Algorithm | Why |
| :--- | :--- | :--- |
| **Nginx** | Radix Tree (character-based) | Needs character-level prefix matching for `location /static/` blocks |
| **Envoy** | Trie + Linear scan fallback | Trie for exact/prefix, linear scan for regex routes |
| **Kong** | Radix Tree (via lua-resty-router) | OpenResty-based, needs Nginx compatibility |
| **Express.js** | Layer stack (linear scan) | Simpler but $O(N)$ — fine for Node.js scale |
| **Gin (Go)** | Radix Tree (httprouter) | Extremely optimized, zero-allocation routing |
| **GatePulse** | Segment-based Trie | Clean, interview-explainable, $O(k)$ with backtracking |

### Key Insight for Interviews:
When an interviewer asks "how would you design a URL router?", they're testing:
1. Whether you know that **HashMap alone cannot handle parameterized routes**.
2. Whether you understand **Trie data structures** beyond textbook autocomplete.
3. Whether you can reason about **match priority and ambiguity resolution**.
4. Whether you understand the **time-space trade-offs** vs. regex-based alternatives.
