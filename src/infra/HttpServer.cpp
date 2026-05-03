#include "HttpServer.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
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
    body { font-family: Arial, sans-serif; margin: 24px; line-height: 1.4; }
    textarea { width: 100%; height: 320px; }
    .row { margin-bottom: 12px; }
    .status { padding: 8px; background: #f2f2f2; }
    .form { border: 1px solid #ddd; padding: 12px; margin: 8px 0; }
    input { width: 100%; padding: 6px; margin: 4px 0; }
    button { padding: 8px 16px; }
  </style>
</head>
<body>
  <h1>Catch the Letter</h1>
  <div class="row status" id="status">loading...</div>
  <div class="row">
    <h3>Events</h3>
    <pre class="status" id="events">loading...</pre>
  </div>
  <div class="row">
    <h3>Active forms</h3>
    <div id="forms">loading...</div>
  </div>
  <div class="row">
    <h3>Profile (JSON)</h3>
    <textarea id="profile"></textarea>
    <button onclick="saveProfile()">Save profile</button>
    <span id="profile-result"></span>
  </div>
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
      const events = await fetch('/api/events').then(r => r.json()).catch(() => null);
      document.getElementById('events').innerText = events ? JSON.stringify(events, null, 2) : 'events error';
      const forms = await fetch('/api/forms/active').then(r => r.json()).catch(() => []);
      document.getElementById('forms').innerHTML = forms.map(renderForm).join('') || 'no active forms';
      const profile = await fetch('/api/profile').then(r => r.text()).catch(() => '');
      if (!document.getElementById('profile').matches(':focus')) document.getElementById('profile').value = profile;
      const rules = await fetch('/api/rules').then(r => r.text()).catch(() => '');
      if (!document.getElementById('rules').matches(':focus')) document.getElementById('rules').value = rules;
    }
    function renderForm(f) {
      const fields = (f.fields || []).map((field, i) => `
        <label>${i + 1}. ${field.label || field.id}<input value="${field.value || ''}"
          onchange="updateField('${f.id}', '${field.id}', this.value)"></label>
      `).join('');
      return `<div class="form"><b>${f.title || f.form_url}</b><br>Status: ${f.status}<br>${f.form_url}
        <div>${fields}</div>
        <button onclick="fillForm('${f.id}')">Fill</button>
        <button onclick="submitForm('${f.id}')">Submit</button>
        <button onclick="manualForm('${f.id}')">Manual</button>
      </div>`;
    }
    async function saveRules() {
      const body = document.getElementById('rules').value;
      const res = await fetch('/api/rules', {method:'POST', body});
      const text = await res.text();
      document.getElementById('save-result').innerText = res.ok ? 'saved' : ('error: ' + text);
    }
    async function saveProfile() {
      const body = document.getElementById('profile').value;
      const res = await fetch('/api/profile', {method:'POST', body});
      document.getElementById('profile-result').innerText = res.ok ? 'saved' : ('error');
    }
    async function updateField(id, field, value) {
      await fetch(`/api/forms/${id}/field`, {method:'POST', body: JSON.stringify({field_id: field, value})});
    }
    async function fillForm(id) { await fetch(`/api/forms/${id}/fill`, {method:'POST'}); load(); }
    async function submitForm(id) { await fetch(`/api/forms/${id}/submit`, {method:'POST'}); load(); }
    async function manualForm(id) { await fetch(`/api/forms/${id}/manual`, {method:'POST'}); load(); }
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

    server.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
      int limit = 0;
      if (req.has_param("limit")) {
        try {
          limit = std::stoi(req.get_param_value("limit"));
        } catch (...) {
          limit = 0;
        }
      }
      std::string body = handlers.get_events_json ? handlers.get_events_json(limit) : "[]";
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

    server.Get("/api/forms/active", [this](const httplib::Request&, httplib::Response& res) {
      std::string body = handlers.get_active_forms_json ? handlers.get_active_forms_json() : "[]";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Get(R"(/api/forms/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
      std::string body = handlers.get_form_json ? handlers.get_form_json(req.matches[1]) : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post(R"(/api/forms/([^/]+)/field)", [this](const httplib::Request& req, httplib::Response& res) {
      std::string err;
      bool ok = handlers.update_form_field_json ?
          handlers.update_form_field_json(req.matches[1], req.body, err) : false;
      if (!ok) {
        res.status = 400;
        res.set_content(err.empty() ? "invalid" : err, "text/plain; charset=utf-8");
        return;
      }
      res.set_content("ok", "text/plain; charset=utf-8");
    });

    auto post_form_action = [this](const httplib::Request& req,
                                   httplib::Response& res,
                                   const std::function<bool(const std::string&, std::string&)>& fn) {
      std::string err;
      bool ok = fn ? fn(req.matches[1], err) : false;
      if (!ok) {
        res.status = 400;
        res.set_content(err.empty() ? "failed" : err, "text/plain; charset=utf-8");
        return;
      }
      res.set_content("ok", "text/plain; charset=utf-8");
    };

    server.Post(R"(/api/forms/([^/]+)/fill)", [this, post_form_action](const httplib::Request& req, httplib::Response& res) {
      post_form_action(req, res, handlers.fill_form);
    });
    server.Post(R"(/api/forms/([^/]+)/submit)", [this, post_form_action](const httplib::Request& req, httplib::Response& res) {
      post_form_action(req, res, handlers.submit_form);
    });
    server.Post(R"(/api/forms/([^/]+)/manual)", [this, post_form_action](const httplib::Request& req, httplib::Response& res) {
      post_form_action(req, res, handlers.manual_form);
    });
    server.Post(R"(/api/forms/([^/]+)/cancel)", [this, post_form_action](const httplib::Request& req, httplib::Response& res) {
      post_form_action(req, res, handlers.cancel_form);
    });
    server.Post(R"(/api/forms/([^/]+)/reinspect)", [this, post_form_action](const httplib::Request& req, httplib::Response& res) {
      post_form_action(req, res, handlers.reinspect_form);
    });
    server.Post(R"(/api/forms/([^/]+)/auth/credentials)", [this](const httplib::Request& req, httplib::Response& res) {
      std::string err;
      bool ok = handlers.auth_credentials ? handlers.auth_credentials(req.matches[1], req.body, err) : false;
      res.status = ok ? 200 : 400;
      res.set_content(nlohmann::json({{"ok", ok}, {"error", err}}).dump(), "application/json; charset=utf-8");
    });
    server.Post(R"(/api/forms/([^/]+)/auth/2fa)", [this](const httplib::Request& req, httplib::Response& res) {
      std::string err;
      bool ok = handlers.auth_two_factor ? handlers.auth_two_factor(req.matches[1], req.body, err) : false;
      res.status = ok ? 200 : 400;
      res.set_content(nlohmann::json({{"ok", ok}, {"error", err}}).dump(), "application/json; charset=utf-8");
    });

    server.Get("/api/profile", [this](const httplib::Request&, httplib::Response& res) {
      std::string body = handlers.get_profile_json ? handlers.get_profile_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post("/api/profile", [this](const httplib::Request& req, httplib::Response& res) {
      std::string err;
      bool ok = handlers.set_profile_json ? handlers.set_profile_json(req.body, err) : false;
      if (!ok) {
        res.status = 400;
        res.set_content(err.empty() ? "invalid" : err, "text/plain; charset=utf-8");
        return;
      }
      res.set_content("ok", "text/plain; charset=utf-8");
    });

    server.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
      std::string body = handlers.get_config_json ? handlers.get_config_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post("/api/test/browser", [this](const httplib::Request&, httplib::Response& res) {
      res.set_content(handlers.test_browser_json ? handlers.test_browser_json() : "{\"ok\":false}",
                      "application/json; charset=utf-8");
    });
    server.Post("/api/test/llm", [this](const httplib::Request&, httplib::Response& res) {
      res.set_content(handlers.test_llm_json ? handlers.test_llm_json() : "{\"ok\":false}",
                      "application/json; charset=utf-8");
    });
    server.Post("/api/test/telegram", [this](const httplib::Request&, httplib::Response& res) {
      res.set_content(handlers.test_telegram_json ? handlers.test_telegram_json() : "{\"ok\":false}",
                      "application/json; charset=utf-8");
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
