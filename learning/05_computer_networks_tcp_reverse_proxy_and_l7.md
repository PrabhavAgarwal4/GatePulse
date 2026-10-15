# Learning Note 05: Computer Networks — L7 Reverse Proxying, Winsock RAII, & Stream Framing

This document explains the deep "WHYs" behind every design and architectural decision made in Milestone 5 of **GatePulse**.

---

## 1. WHAT is the difference between an L4 (Layer 4) and an L7 (Layer 7) Reverse Proxy?

### The OSI Model Perspective:
- **L4 (Transport Layer):** Operates strictly on IP packets and TCP/UDP ports.
  - Examples: AWS Network Load Balancer (NLB), HAProxy (in TCP mode), Linux LVS (`ipvs`).
  - L4 proxies do **not inspect or modify application payloads**. A packet arriving on port 80/443 is forwarded via TCP connection splicing (or NAT) directly to the backend.
  - Advantage: Blazing speed, minimal CPU overhead.
  - Limitation: **Cannot inspect URL paths, cannot read HTTP headers, cannot parse cookies, and cannot extract JWT tokens**.
- **L7 (Application Layer):** Operates on the full HTTP protocol semantics.
  - Examples: GatePulse, Nginx, Envoy, AWS Application Load Balancer (ALB).
  - The gateway acts as an **HTTP terminator**: It establishes a TCP connection with the client, buffers and parses the HTTP request, makes intelligent routing/rate-limiting/security decisions, and then establishes an independent TCP connection to an upstream backend server.

### Comparison Matrix for Placement Interviews:

| Dimension | Layer 4 (TCP/UDP Proxy) | Layer 7 (HTTP/gRPC Gateway - GatePulse) |
| :--- | :--- | :--- |
| **Protocol Awareness** | TCP / UDP packet streams | HTTP/1.1, HTTP/2, WebSocket, gRPC |
| **Routing Capability** | IP address & Port only | URL Path (`/api/v1/users/:id`), HTTP Method, Headers, Query Params |
| **Security & Admission** | IP Rate limiting / SYN Flood defense | Token Bucket per IP, Bearer Token Auth, CORS preflight |
| **Buffering Overhead** | Low (Zero-copy kernel packet forwarding) | Moderate (Must parse and buffer HTTP headers and bodies) |
| **Header Injection** | Impossible | Injects `X-Proxy-By`, `X-Forwarded-For`, `X-Response-Time-Ms` |

---

## 2. WHY Winsock RAII and how do we prevent Socket Exhaustion?

### The Problem: File Descriptor / Handle Leaks
In Windows (Winsock2) and Linux (POSIX sockets), open sockets consume OS kernel resources:
- Each socket has an associated Send Buffer and Receive Buffer allocated in kernel non-paged pool memory.
- If an exception occurs, a connection times out, or a function returns early without calling `closesocket(fd)`, the socket descriptor remains open in the OS.
- Under high throughput (10,000 req/sec), the process hits the OS limit (`WSAENOBUFS` / `EMFILE`), and the gateway crashes or hangs.

### The Solution: `SocketRAII` Move-Only Wrapper
```cpp
class SocketRAII {
    SOCKET fd_;
public:
    explicit SocketRAII(SOCKET fd) : fd_(fd) {}
    ~SocketRAII() { close(); }

    // Move constructor transfers ownership
    SocketRAII(SocketRAII&& other) noexcept : fd_(other.fd_) {
        other.fd_ = INVALID_SOCKET;
    }
    // Copy constructor deleted to prevent duplicate closes
    SocketRAII(const SocketRAII&) = delete;

    void close() {
        if (fd_ != INVALID_SOCKET) {
            closesocket(fd_);
            fd_ = INVALID_SOCKET;
        }
    }
};
```
1. **Guaranteed Destruction:** Regardless of whether code paths exit via return, break, or thrown exceptions, the destructor automatically invokes `closesocket(fd_)`.
2. **Move-Only Semantics:** Sockets cannot be inadvertently duplicated across threads. Ownership is strictly transferred.

---

## 3. WHAT is `SO_REUSEADDR` and WHY is it Critical?

### The TIME_WAIT State in TCP:
When an endpoint closes an active TCP connection, TCP's Four-Way Handshake enters the **`TIME_WAIT`** state (often lingering for 30 to 120 seconds):
$$\text{TIME\_WAIT Duration} = 2 \times \text{MSL} \text{ (Maximum Segment Lifetime)}$$
- **Why TCP does this:** To ensure any delayed duplicate packets wandering across the Internet are drained and do not corrupt a newly opened connection on the same IP:port.
- **The Problem for Gateways:** If GatePulse restarts or binds to a local port that recently handled traffic, `bind()` fails with `WSAEADDRINUSE` (Address already in use).

### The Fix:
```cpp
BOOL opt = TRUE;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
```
`SO_REUSEADDR` tells the OS kernel: *"Allow this socket to bind immediately, even if a previous connection on this port is still in TIME_WAIT."*

---

## 4. HOW does HTTP Stream Framing Work over Raw TCP?

### The Core Networking Reality:
**TCP is a continuous stream of bytes, NOT a message protocol.**
TCP has no concept of "packets" or "requests" at the application layer.
- A single 500-byte HTTP request might arrive in five separate 100-byte chunks (`recv()`).
- Or two back-to-back HTTP requests might arrive combined inside a single `recv()` call.

### How GatePulse Parses HTTP Framing:
1. **Header Boundary Detection:**
   The HTTP/1.1 specification dictates that headers end at the first four-byte delimiter: `\r\n\r\n` (CRLF CRLF) or `\n\n`.
2. **Body Length Detection:**
   Once headers are parsed, the gateway inspects:
   - `Content-Length: <bytes>`: Reads exactly $N$ bytes of body payload.
   - `Transfer-Encoding: chunked`: Reads dynamic size chunks followed by CRLF until a 0-length chunk.
3. **Pipelining & End-of-Stream:**
   If a client closes the connection (`recv() == 0`), EOF is signaled.

---

## 5. WHY Connection Timeouts (`SO_RCVTIMEO` / `SO_SNDTIMEO`) Prevent Slowloris Attacks

### What is a Slowloris Attack?
An attacker opens hundreds of TCP connections to the gateway and sends HTTP headers at a crawl (e.g. 1 byte every 15 seconds):
```
GET / HTTP/1.1\r\n
Host: example.com\r\n
X-Custom-Header-1: a... (wait 15s) ...b
```
Without timeouts, the gateway worker threads block indefinitely on `recv()`, starving all available connection slots and rendering the gateway unreachable for legitimate users.

### GatePulse's Defense:
```cpp
DWORD timeout_ms = 3000; // 3 second timeout
setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
```
If a client or upstream backend fails to send data within the configured window, `recv()` aborts immediately with `WSAETIMEDOUT`, reclaiming thread and socket resources.
