#pragma once

#include "arch.h"
#include "config.h"
#include "fs.h"
#include "net.h"
#include "str.h"

struct mg_http_header {
  struct mg_str name;   // Header name
  struct mg_str value;  // Header value
};

struct mg_http_message {
  struct mg_str method, uri, query, proto;             // Request/response line
  struct mg_http_header headers[MG_MAX_HTTP_HEADERS];  // Headers
  struct mg_str body;                                  // Body
  struct mg_str head;                                  // Request + headers
  struct mg_str message;  // Request + headers + body
};

enum mg_rate_limit_algorithm {
  MG_RATELIMIT_ALGO_TOKEN_BUCKET = 0,
  MG_RATELIMIT_ALGO_SLIDING_WINDOW = 1,
  MG_RATELIMIT_ALGO_FIXED_WINDOW = 2,
};

struct mg_rate_limit_rule {
  enum mg_rate_limit_algorithm algorithm;  // Algorithm used by this rule
  double rate;                             // Token bucket refill rate, tokens/sec
  double capacity;                         // Token bucket capacity (burst)
  uint32_t window_size;                    // Sliding/fixed window size, seconds
  uint32_t max_requests;                   // Sliding/fixed max requests per window
};

typedef bool (*mg_rate_limit_whitelist_fn)(struct mg_connection *c,
                                           struct mg_http_message *hm,
                                           void *userdata);

struct mg_rate_limit_opts {
  struct mg_rate_limit_rule global;     // Global rate limit, all requests
  struct mg_rate_limit_rule per_ip;     // Per source IP limit
  struct mg_rate_limit_rule per_route;  // Per route limit
  size_t max_entries;                   // Max in-memory entries
  uint64_t cleanup_interval_ms;         // Stale entry cleanup interval
  uint32_t stale_after_sec;             // Entry inactivity expiration
  mg_rate_limit_whitelist_fn whitelist; // Whitelist callback, optional
  void *whitelist_data;                 // User data for whitelist callback
};

// Internal helper result type used by HTTP serving code
struct mg_rate_limit_result {
  bool allowed;
  uint32_t limit;
  uint32_t remaining;
  uint32_t reset;
  uint32_t retry_after;
};

// Parameter for mg_http_serve_dir()
struct mg_http_serve_opts {
  const char *root_dir;       // Web root directory, must be non-NULL
  const char *ssi_pattern;    // SSI file name pattern, e.g. #.shtml
  const char *extra_headers;  // Extra HTTP headers to add in responses
  const char *mime_types;     // Extra mime types, ext1=type1,ext2=type2,..
  const char *page404;        // Path to the 404 page, or NULL by default
  struct mg_fs *fs;           // Filesystem implementation. Use NULL for POSIX
  struct mg_rate_limit_opts *rate_limit;  // Optional rate limiting settings
};

bool mg_rate_limit_check(struct mg_connection *c, struct mg_http_message *hm,
                         const struct mg_rate_limit_opts *opts,
                         struct mg_rate_limit_result *res);

// Parameter for mg_http_next_multipart
struct mg_http_part {
  struct mg_str name;      // Form field name
  struct mg_str filename;  // Filename for file uploads
  struct mg_str body;      // Part contents
};

int mg_http_parse(const char *s, size_t len, struct mg_http_message *);
int mg_http_get_request_len(const unsigned char *buf, size_t buf_len);
void mg_http_printf_chunk(struct mg_connection *cnn, const char *fmt, ...);
void mg_http_write_chunk(struct mg_connection *c, const char *buf, size_t len);
struct mg_connection *mg_http_listen(struct mg_mgr *, const char *url,
                                     mg_event_handler_t fn, void *fn_data);
struct mg_connection *mg_http_connect(struct mg_mgr *, const char *url,
                                      mg_event_handler_t fn, void *fn_data);
void mg_http_serve_dir(struct mg_connection *, struct mg_http_message *hm,
                       const struct mg_http_serve_opts *);
void mg_http_serve_file(struct mg_connection *, struct mg_http_message *hm,
                        const char *path, const struct mg_http_serve_opts *);
void mg_http_reply(struct mg_connection *, int status_code, const char *headers,
                   const char *body_fmt, ...);
struct mg_str *mg_http_get_header(struct mg_http_message *, const char *name);
struct mg_str mg_http_var(struct mg_str buf, struct mg_str name);
int mg_http_get_var(const struct mg_str *, const char *name, char *, size_t);
int mg_url_decode(const char *s, size_t n, char *to, size_t to_len, int form);
size_t mg_url_encode(const char *s, size_t n, char *buf, size_t len);
void mg_http_creds(struct mg_http_message *, char *, size_t, char *, size_t);
long mg_http_upload(struct mg_connection *c, struct mg_http_message *hm,
                    struct mg_fs *fs, const char *dir, size_t max_size);
void mg_http_bauth(struct mg_connection *, const char *user, const char *pass);
struct mg_str mg_http_get_header_var(struct mg_str s, struct mg_str v);
size_t mg_http_next_multipart(struct mg_str, size_t, struct mg_http_part *);
int mg_http_status(const struct mg_http_message *hm);
