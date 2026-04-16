#include "rate_limit.h"

#include "util.h"

enum mg_rate_limit_key_type { MG_RL_KEY_GLOBAL, MG_RL_KEY_IP, MG_RL_KEY_ROUTE };

struct mg_rate_limit_entry {
  struct mg_rate_limit_entry *next;
  const struct mg_rate_limit_opts *owner;
  enum mg_rate_limit_key_type type;
  bool ip6;
  uint8_t ip[16];
  uint32_t route_hash;
  uint64_t last_seen_ms;
  double tokens;
  uint64_t last_refill_ms;
  uint64_t fw_start_ms;
  uint32_t fw_count;
  uint64_t *sw_times;
  uint32_t sw_count;
  uint32_t sw_cap;
};

static struct mg_rate_limit_entry *s_entries = NULL;
static size_t s_entry_count = 0;
static uint64_t s_last_cleanup = 0;

static uint32_t mg_rl_hash_uri(struct mg_str uri) {
  return mg_crc32(0, uri.buf, uri.len);
}

static bool mg_rl_rule_enabled(const struct mg_rate_limit_rule *r) {
  if (r == NULL) return false;
  if (r->algorithm == MG_RATELIMIT_ALGO_TOKEN_BUCKET) {
    return r->rate > 0.0 && r->capacity > 0.0;
  }
  if (r->algorithm == MG_RATELIMIT_ALGO_FIXED_WINDOW ||
      r->algorithm == MG_RATELIMIT_ALGO_SLIDING_WINDOW) {
    return r->window_size > 0 && r->max_requests > 0;
  }
  return false;
}

static bool mg_rl_key_matches(const struct mg_rate_limit_entry *e,
                              const struct mg_rate_limit_opts *opts,
                              enum mg_rate_limit_key_type type,
                              const struct mg_connection *c,
                              const struct mg_http_message *hm) {
  if (e->owner != opts || e->type != type) return false;
  if (type == MG_RL_KEY_GLOBAL) return true;
  if (type == MG_RL_KEY_ROUTE) return e->route_hash == mg_rl_hash_uri(hm->uri);
  if (e->ip6 != c->rem.is_ip6) return false;
  return memcmp(e->ip, c->rem.addr.ip, c->rem.is_ip6 ? 16 : 4) == 0;
}

static void mg_rl_free_entry(struct mg_rate_limit_entry *e) {
  mg_free(e->sw_times);
  mg_free(e);
}

static void mg_rl_cleanup(uint64_t now_ms, const struct mg_rate_limit_opts *opts) {
  uint64_t interval =
      opts->cleanup_interval_ms > 0 ? opts->cleanup_interval_ms
                                    : MG_RATELIMIT_CLEANUP_INTERVAL;
  uint64_t stale_ms =
      (uint64_t) (opts->stale_after_sec > 0 ? opts->stale_after_sec
                                            : MG_RATELIMIT_STALE_SECONDS) *
      1000;
  struct mg_rate_limit_entry **pp = &s_entries;
  if (now_ms - s_last_cleanup < interval) return;
  s_last_cleanup = now_ms;
  while (*pp != NULL) {
    struct mg_rate_limit_entry *e = *pp;
    if (now_ms > e->last_seen_ms && now_ms - e->last_seen_ms > stale_ms) {
      *pp = e->next;
      mg_rl_free_entry(e);
      if (s_entry_count > 0) s_entry_count--;
    } else {
      pp = &(*pp)->next;
    }
  }
}

static void mg_rl_evict_one(void) {
  struct mg_rate_limit_entry *victim = NULL, *p = s_entries, *prev = NULL;
  struct mg_rate_limit_entry *victim_prev = NULL;
  while (p != NULL) {
    if (victim == NULL || p->last_seen_ms < victim->last_seen_ms) {
      victim = p;
      victim_prev = prev;
    }
    prev = p;
    p = p->next;
  }
  if (victim != NULL) {
    if (victim_prev != NULL) {
      victim_prev->next = victim->next;
    } else {
      s_entries = victim->next;
    }
    mg_rl_free_entry(victim);
    if (s_entry_count > 0) s_entry_count--;
  }
}

static struct mg_rate_limit_entry *mg_rl_get_entry(
    const struct mg_rate_limit_opts *opts, enum mg_rate_limit_key_type type,
    const struct mg_connection *c, const struct mg_http_message *hm,
    uint64_t now_ms) {
  struct mg_rate_limit_entry *e = s_entries;
  while (e != NULL) {
    if (mg_rl_key_matches(e, opts, type, c, hm)) {
      e->last_seen_ms = now_ms;
      return e;
    }
    e = e->next;
  }
  {
    size_t max_entries =
        opts->max_entries > 0 ? opts->max_entries : MG_RATELIMIT_MAX_ENTRIES;
    e = (struct mg_rate_limit_entry *) mg_calloc(1, sizeof(*e));
    if (e == NULL) return NULL;
    while (s_entry_count >= max_entries && s_entries != NULL) mg_rl_evict_one();
    e->owner = opts;
    e->type = type;
    e->ip6 = c->rem.is_ip6;
    memcpy(e->ip, c->rem.addr.ip, c->rem.is_ip6 ? 16 : 4);
    e->route_hash = mg_rl_hash_uri(hm->uri);
    e->last_seen_ms = now_ms;
    e->next = s_entries;
    s_entries = e;
    s_entry_count++;
    return e;
  }
}

static bool mg_rl_apply_token_bucket(struct mg_rate_limit_entry *e,
                                     const struct mg_rate_limit_rule *r,
                                     uint64_t now_ms,
                                     struct mg_rate_limit_result *res) {
  double elapsed = 0.0;
  if (e->last_refill_ms == 0) {
    e->tokens = r->capacity;
    e->last_refill_ms = now_ms;
  }
  if (now_ms > e->last_refill_ms) {
    elapsed = (double) (now_ms - e->last_refill_ms) / 1000.0;
    e->tokens += elapsed * r->rate;
    if (e->tokens > r->capacity) e->tokens = r->capacity;
    e->last_refill_ms = now_ms;
  }
  res->limit = (uint32_t) r->capacity;
  if (e->tokens >= 1.0) {
    e->tokens -= 1.0;
    res->allowed = true;
    res->remaining = (uint32_t) e->tokens;
    res->reset = e->tokens >= r->capacity ? 0 : 1;
    res->retry_after = 0;
  } else {
    double need = (1.0 - e->tokens) / r->rate;
    uint32_t wait = (uint32_t) (need + 0.999);
    res->allowed = false;
    res->remaining = 0;
    res->retry_after = wait;
    res->reset = wait;
  }
  return res->allowed;
}

static bool mg_rl_apply_fixed_window(struct mg_rate_limit_entry *e,
                                     const struct mg_rate_limit_rule *r,
                                     uint64_t now_ms,
                                     struct mg_rate_limit_result *res) {
  uint64_t ws = (uint64_t) r->window_size * 1000;
  if (e->fw_start_ms == 0 || now_ms - e->fw_start_ms >= ws) {
    e->fw_start_ms = now_ms;
    e->fw_count = 0;
  }
  res->limit = r->max_requests;
  if (e->fw_count < r->max_requests) {
    e->fw_count++;
    res->allowed = true;
    res->remaining = r->max_requests - e->fw_count;
    res->retry_after = 0;
  } else {
    res->allowed = false;
    res->remaining = 0;
    res->retry_after = (uint32_t) ((ws - (now_ms - e->fw_start_ms) + 999) / 1000);
  }
  res->reset = (uint32_t) ((ws - (now_ms - e->fw_start_ms) + 999) / 1000);
  return res->allowed;
}

static bool mg_rl_apply_sliding_window(struct mg_rate_limit_entry *e,
                                       const struct mg_rate_limit_rule *r,
                                       uint64_t now_ms,
                                       struct mg_rate_limit_result *res) {
  uint64_t ws = (uint64_t) r->window_size * 1000;
  uint32_t i = 0;
  if (e->sw_cap < r->max_requests) {
    uint64_t *tmp = (uint64_t *) mg_calloc(r->max_requests, sizeof(uint64_t));
    if (tmp == NULL) {
      res->allowed = true;
      res->limit = r->max_requests;
      res->remaining = r->max_requests > 0 ? r->max_requests - 1 : 0;
      res->reset = 0;
      res->retry_after = 0;
      return true;
    }
    if (e->sw_times != NULL && e->sw_count > 0) {
      memcpy(tmp, e->sw_times, e->sw_count * sizeof(uint64_t));
    }
    mg_free(e->sw_times);
    e->sw_times = tmp;
    e->sw_cap = r->max_requests;
  }
  while (i < e->sw_count && now_ms - e->sw_times[i] >= ws) i++;
  if (i > 0 && i < e->sw_count) {
    memmove(e->sw_times, e->sw_times + i, (e->sw_count - i) * sizeof(uint64_t));
  }
  if (i > 0) e->sw_count -= i;
  res->limit = r->max_requests;
  if (e->sw_count < r->max_requests) {
    e->sw_times[e->sw_count++] = now_ms;
    res->allowed = true;
    res->remaining = r->max_requests - e->sw_count;
    res->retry_after = 0;
    res->reset = e->sw_count > 0 ? r->window_size : 0;
  } else {
    uint64_t oldest = e->sw_times[0];
    res->allowed = false;
    res->remaining = 0;
    res->retry_after = (uint32_t) ((ws - (now_ms - oldest) + 999) / 1000);
    res->reset = res->retry_after;
  }
  return res->allowed;
}

static bool mg_rl_apply_rule(struct mg_rate_limit_entry *e,
                             const struct mg_rate_limit_rule *r, uint64_t now_ms,
                             struct mg_rate_limit_result *res) {
  memset(res, 0, sizeof(*res));
  if (r->algorithm == MG_RATELIMIT_ALGO_TOKEN_BUCKET) {
    return mg_rl_apply_token_bucket(e, r, now_ms, res);
  } else if (r->algorithm == MG_RATELIMIT_ALGO_SLIDING_WINDOW) {
    return mg_rl_apply_sliding_window(e, r, now_ms, res);
  } else {
    return mg_rl_apply_fixed_window(e, r, now_ms, res);
  }
}

static bool mg_rl_merge_result(struct mg_rate_limit_result *dst,
                               const struct mg_rate_limit_result *src) {
  if (dst->limit == 0 || src->limit < dst->limit) dst->limit = src->limit;
  if (dst->limit == src->limit) dst->remaining = src->remaining;
  if (src->reset > dst->reset) dst->reset = src->reset;
  if (src->retry_after > dst->retry_after) dst->retry_after = src->retry_after;
  if (!src->allowed) dst->allowed = false;
  return dst->allowed;
}

bool mg_rate_limit_check(struct mg_connection *c, struct mg_http_message *hm,
                         const struct mg_rate_limit_opts *opts,
                         struct mg_rate_limit_result *res) {
  uint64_t now_ms = mg_millis();
  struct mg_rate_limit_result tmp, out;
  if (res != NULL) memset(res, 0, sizeof(*res));
  if (opts == NULL) return true;
  if (opts->whitelist != NULL && opts->whitelist(c, hm, opts->whitelist_data)) {
    if (res != NULL) res->allowed = true;
    return true;
  }
  mg_rl_cleanup(now_ms, opts);
  memset(&out, 0, sizeof(out));
  out.allowed = true;

  if (mg_rl_rule_enabled(&opts->global)) {
    struct mg_rate_limit_entry *e =
        mg_rl_get_entry(opts, MG_RL_KEY_GLOBAL, c, hm, now_ms);
    if (e != NULL) {
      mg_rl_apply_rule(e, &opts->global, now_ms, &tmp);
      mg_rl_merge_result(&out, &tmp);
    }
  }
  if (mg_rl_rule_enabled(&opts->per_ip)) {
    struct mg_rate_limit_entry *e =
        mg_rl_get_entry(opts, MG_RL_KEY_IP, c, hm, now_ms);
    if (e != NULL) {
      mg_rl_apply_rule(e, &opts->per_ip, now_ms, &tmp);
      mg_rl_merge_result(&out, &tmp);
    }
  }
  if (mg_rl_rule_enabled(&opts->per_route)) {
    struct mg_rate_limit_entry *e =
        mg_rl_get_entry(opts, MG_RL_KEY_ROUTE, c, hm, now_ms);
    if (e != NULL) {
      mg_rl_apply_rule(e, &opts->per_route, now_ms, &tmp);
      mg_rl_merge_result(&out, &tmp);
    }
  }
  if (res != NULL) *res = out;
  return out.allowed;
}
