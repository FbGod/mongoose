# AGENTS.md – Rate Limiting Implementation for Mongoose

This document provides guidance for Cursor AI to implement a robust rate limiting system for the Mongoose HTTP server. Follow these instructions precisely to ensure the implementation aligns with the project's architecture, coding standards, and testing requirements.

## 1. Project Context

**Mongoose** is a lightweight, single-file embedded web server library written in C. The project uses an amalgamated build (mongoose.c/mongoose.h) generated from source modules in `src/`. The code follows a modular, event-driven design with minimal external dependencies.

The user has provided:
- A detailed project structure diagram.
- A call stack for HTTP request processing.
- Key data structures and injection points for rate limiting.
- A formal task specification (TOR) in Russian.

You are to implement all required functionality **exclusively using Cursor AI** based on the instructions below.

## 2. Task Summary

Implement a production-ready rate limiting mechanism for the Mongoose HTTP server with the following features:

- **Three algorithms**: Token Bucket, Sliding Window, Fixed Window.
- **Configurable limits**: Global, per-route, per-IP.
- **Response headers**: `Retry-After`, `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`.
- **HTTP 429 status code** when limits are exceeded.
- **In-memory storage** (hash table keyed by client IP) with periodic cleanup and memory bounds.
- **Whitelisting capability** to exempt certain connections.
- **Integration** with existing `mg_http_listen` and `mg_http_serve_opts`.
- **Comprehensive tests**: Unit, integration, and stress tests.

## 3. Detailed Requirements

Refer to the TOR in the user prompt. Key points:

- **Algorithms**:
  - **Token Bucket**: `rate` (tokens per second), `capacity` (burst).
  - **Sliding Window**: `window_size` (seconds), `max_requests`.
  - **Fixed Window**: `window_size` (seconds), `max_requests`.

- **Configuration via `mg_http_serve_opts`**:
  Add a new field `struct mg_rate_limit_opts *rate_limit`.

- **Behavior on limit exceeded**:
  - Respond with `429 Too Many Requests`.
  - Include `Retry-After` header (seconds until reset).
  - Close connection or keep-alive based on existing logic.

- **State storage**:
  - Use a hash table with LRU-like eviction to limit memory.
  - Cleanup stale entries periodically (e.g., every minute).

- **API additions** (in `mongoose.h` public interface):
  - `struct mg_rate_limit_opts` with algorithm enum and parameters.
  - `mg_rate_limit_check()` – internal use; not directly exposed to user.
  - `mg_http_serve_opts` extended with `rate_limit` pointer.

## 4. Integration Points in Existing Codebase

Based on the provided diagrams, modify these files:

| File | Changes |
|------|---------|
| `src/http.h` | Add `struct mg_rate_limit_opts` declaration and a pointer field in `struct mg_http_serve_opts`. |
| `src/http.c` | Inside `mg_http_serve_dir()` and/or `http_cb()` before `MG_EV_HTTP_MSG`, call rate limit check. |
| `src/config.h` | Define default values like `MG_RATELIMIT_MAX_ENTRIES`, `MG_RATELIMIT_CLEANUP_INTERVAL`. |
| `mongoose.h` (generated) | The amalgamated header will include the new API; but you'll modify `src/` files. |

**Injection point recommendation**: Perform the check in `http_cb()` right after parsing headers and before dispatching `MG_EV_HTTP_MSG`. This allows whitelisting based on headers (e.g., `X-API-Key`) and ensures consistent handling for all HTTP requests served via `mg_http_serve_*`.

## 5. New Files to Create

| File | Purpose |
|------|---------|
| `src/rate_limit.h` | Internal header with algorithm structs, hash table entry, function prototypes. |
| `src/rate_limit.c` | Implementation of all three algorithms, hash table storage, cleanup logic, and `mg_rate_limit_check()`. |
| `tutorials/http/rate-limit-server/` | Example demonstrating usage with different algorithms. |
| `test/rate_limit_test.c` | Standalone unit tests (or integrate into `test/unit_test.c`). |

Ensure these new files are included in the build system (e.g., update `Makefile` or `CMakeLists.txt` appropriately). For the amalgamated build, you may need to modify `mklib.py` (the amalgamation script) to include the new `.c` file.

## 6. Implementation Plan (Phases)

### Phase 1: Core Data Structures and Hash Table
- Define `struct mg_rate_limit_key` (IP address + port? Or just IP? Use `c->rem` address string).
- Define `struct mg_rate_limit_entry` containing counters/timestamps for the chosen algorithm plus last access time.
- Implement a fixed-size hash table with chaining and LRU eviction.
- Implement cleanup thread/timer (or integrate into `mg_mgr_poll()` to avoid threading complexity – Mongoose is single-threaded, so do cleanup inside poll periodically).

### Phase 2: Algorithm Implementations
- Token Bucket: `tokens` float, `last_refill` timestamp.
- Sliding Window: circular buffer of timestamps (or deque of request times).
- Fixed Window: `count` and `window_start` timestamp.
- All algorithm functions should be stateless and take `entry` pointer.

### Phase 3: Integration with HTTP Processing
- In `http.c`, after `mg_http_parse()` and before calling user handler, check if `opts->rate_limit` is set.
- Extract client IP from `c->rem` (use `mg_ntoa(&c->rem, ...)`).
- Call internal `mg_rate_limit_check()`.
- If limit exceeded, construct a 429 response and return (do not call user handler).
- Set appropriate rate-limit headers on both allowed and denied responses.

### Phase 4: Configuration and API
- Add `MG_RATELIMIT_ALGO_*` enum.
- Add `struct mg_rate_limit_opts` with `algorithm`, `rate`, `capacity`, `window_size`, `max_requests`, `whitelist_cb` (optional user callback to bypass).
- Modify `mg_http_serve_opts` definition in `http.h`.
- Ensure backward compatibility (if `rate_limit` is NULL, no checks).

### Phase 5: Testing
- Unit tests for each algorithm.
- Test hash table eviction and cleanup.
- Integration test: spawn a simple HTTP server with rate limit, send multiple requests, verify 429 and headers.
- Stress test: many clients (simulated) to ensure no memory leaks or crashes.

## 7. Code Conventions (Mongoose Style)

- Use `mg_` prefix for all public symbols.
- Use `MG_` prefix for macros.
- Function naming: `mg_<module>_<action>()` (e.g., `mg_rate_limit_check`).
- Structures are `struct mg_...`.
- Error handling: return codes, no exceptions.
- Minimal dynamic allocation: prefer static arrays where possible, or use `mg_calloc`/`mg_free`.
- Comments: Doxygen style `/** ... */` for public API.
- Keep code portable (C99, no POSIX-specific unless guarded).
- Avoid global state; attach state to `mg_mgr` if needed.

## 8. Testing Guidelines

- Tests must be runnable with `make test` (existing `test/unit_test.c`).
- Use Mongoose's built-in test framework (e.g., `MG_TEST()` macro).
- Test coverage should include:
  - All algorithms with valid and exhausted states.
  - Cleanup of old entries.
  - Whitelisting.
  - Concurrent connections (simulate with looped calls).
- Provide a separate tutorial example that can be compiled and run.

## 9. Success Criteria

- All code compiles without warnings (using `-Wall -Wextra -Werror`).
- All existing tests pass.
- New tests pass.
- The rate limiting functionality works as described in the TOR.
- The example server demonstrates the feature clearly.
- The implementation is efficient (O(1) per request) and memory bounded.

## 10. Additional Notes

- The user will use **Cursor AI exclusively** for this task. Ensure the generated code is complete and ready for integration.
- If any decision is ambiguous, refer to the Mongoose source code patterns (e.g., how `mg_http_serve_opts` is used, how timers are managed).
- Provide a summary commit message suitable for a pull request.

Proceed with implementation phase by phase, and output the final code changes for review.