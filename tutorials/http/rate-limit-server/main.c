#include "mongoose.h"

static void fn(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    struct mg_http_serve_opts opts;
    struct mg_rate_limit_opts rl;
    memset(&opts, 0, sizeof(opts));
    memset(&rl, 0, sizeof(rl));
    rl.per_ip.algorithm = MG_RATELIMIT_ALGO_TOKEN_BUCKET;
    rl.per_ip.rate = 5.0;
    rl.per_ip.capacity = 10.0;
    opts.root_dir = ".";
    opts.rate_limit = &rl;
    mg_http_serve_dir(c, hm, &opts);
  }
}

int main(void) {
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  mg_http_listen(&mgr, "http://0.0.0.0:8000", fn, NULL);
  for (;;) mg_mgr_poll(&mgr, 1000);
  return 0;
}
