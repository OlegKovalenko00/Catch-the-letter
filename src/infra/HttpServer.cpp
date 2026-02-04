#include "HttpServer.h"

#include <httplib.h>

#include <atomic>
#include <string>
#include <thread>

namespace {

const char* k_index_html = R"HTML(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>Catch the Letter</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 24px; }
    textarea { width: 100%; height: 320px; }
    .row { margin-bottom: 12px; }
    .status { padding: 8px; background: #f2f2f2; }
    button { padding: 8px 16px; }
  </style>
</head>
<body>
  <h1>Catch the Letter</h1>
  <div class="row status" id="status">loading...</div>
  <div class="row">
    <h3>Rules (JSON)</h3>
    <textarea id="rules"></textarea>
  </div>
  <div class="row">
    <button onclick="saveRules()">Save rules</button>
    <span id="save-result"></span>
  </div>
  <script>
    async function load() {
      const status = await fetch('/api/status').then(r => r.json()).catch(() => null);
      document.getElementById('status').innerText = status ? JSON.stringify(status) : 'status error';
      const rules = await fetch('/api/rules').then(r => r.text()).catch(() => '');
      document.getElementById('rules').value = rules;
    }
    async function saveRules() {
      const body = document.getElementById('rules').value;
      const res = await fetch('/api/rules', {method:'POST', body});
      const text = await res.text();
      document.getElementById('save-result').innerText = res.ok ? 'saved' : ('error: ' + text);
    }
    load();
    setInterval(load, 5000);
  </script>
</body>
</html>
)HTML";

}  // namespace

class http_server_impl final : public http_server {
public:
  http_server_impl(http_config cfg, http_handlers handlers)
    : cfg(std::move(cfg)), handlers(std::move(handlers)) {}

  bool start() override {
    if (running) return true;
    if (!cfg.enabled) return true;

    server.Get("/", [this](const httplib::Request&, httplib::Response& res) {
      res.set_content(k_index_html, "text/html; charset=utf-8");
    });

    server.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
      std::string body = handlers.get_status_json ? handlers.get_status_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Get("/api/rules", [this](const httplib::Request&, httplib::Response& res) {
      std::string body = handlers.get_rules_json ? handlers.get_rules_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post("/api/rules", [this](const httplib::Request& req, httplib::Response& res) {
      std::string err;
      bool ok = handlers.set_rules_json ? handlers.set_rules_json(req.body, err) : false;
      if (!ok) {
        res.status = 400;
        res.set_content(err.empty() ? "invalid" : err, "text/plain; charset=utf-8");
        return;
      }
      res.set_content("ok", "text/plain; charset=utf-8");
    });

    running = true;
    worker = std::thread([this]() {
      server.listen(cfg.host.c_str(), cfg.port);
      running = false;
    });
    return true;
  }

  void stop() override {
    if (!running) return;
    server.stop();
    if (worker.joinable()) worker.join();
    running = false;
  }

  ~http_server_impl() override { stop(); }

private:
  http_config cfg;
  http_handlers handlers;
  httplib::Server server;
  std::thread worker;
  std::atomic<bool> running{false};
};

std::unique_ptr<http_server> make_http_server(const http_config& cfg,
                                              const http_handlers& handlers,
                                              std::string& err) {
  err.clear();
  return std::make_unique<http_server_impl>(cfg, handlers);
}
