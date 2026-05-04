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
    textarea { width: 100%; min-height: 220px; }
    .row { margin-bottom: 12px; }
    .status { padding: 8px; background: #f2f2f2; }
    .form { border: 1px solid #ddd; padding: 12px; margin: 8px 0; }
    input, select { width: 100%; padding: 6px; margin: 4px 0; box-sizing: border-box; }
    button { padding: 8px 16px; margin: 4px 4px 4px 0; }
    .error { color: #a40000; white-space: pre-wrap; }
    .ok { color: #116611; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }
    .readonly { opacity: .75; }
  </style>
</head>
<body>
  <h1>Catch the Letter</h1>
  <div class="row">
    <button onclick="createDemo(false)">Create Demo Form</button>
    <button onclick="createDemo(true)">Create Demo Auth Form</button>
    <button onclick="testService('browser')">Test Browser</button>
    <button onclick="testService('llm')">Test LLM</button>
    <button onclick="testService('telegram')">Test Telegram</button>
    <span id="test-result"></span>
  </div>
  <div class="row status" id="status">loading...</div>
  <div class="row grid">
    <div>
      <h3>Events</h3>
      <pre class="status" id="events">loading...</pre>
    </div>
    <div>
      <h3>Config</h3>
      <pre class="status" id="config">loading...</pre>
    </div>
  </div>
  <div class="row" id="form-section">
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
  <div class="row error" id="global-error"></div>
  <script>
    const authToken = new URLSearchParams(location.search).get('token') || '';
    function withToken(path) {
      if (!authToken) return path;
      return path + (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(authToken);
    }
    function esc(s) {
      return String(s ?? '').replace(/[&<>"']/g, ch => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
    }
    async function api(path, opts = {}) {
      const res = await fetch(withToken(path), opts);
      const text = await res.text();
      let data = text;
      try { data = text ? JSON.parse(text) : {}; } catch (_) {}
      if (!res.ok || (data && data.ok === false)) {
        const msg = (data && data.error) ? data.error : text || res.statusText;
        throw new Error(msg);
      }
      return data;
    }
    function showError(err) {
      document.getElementById('global-error').innerText = err ? String(err.message || err) : '';
    }
    async function load() {
      const status = await api('/api/status').catch(e => ({error: e.message}));
      document.getElementById('status').innerText = JSON.stringify(status, null, 2);
      const events = await api('/api/events').catch(e => [{error: e.message}]);
      document.getElementById('events').innerText = JSON.stringify(events, null, 2);
      const config = await api('/api/config').catch(e => ({error: e.message}));
      document.getElementById('config').innerText = JSON.stringify(config, null, 2);
      const forms = await api('/api/forms/active').catch(() => []);
      document.getElementById('forms').innerHTML = forms.map(renderForm).join('') || 'no active forms';
      const profile = await fetch(withToken('/api/profile')).then(r => r.text()).catch(() => '');
      if (!document.getElementById('profile').matches(':focus')) document.getElementById('profile').value = profile;
      const rules = await fetch(withToken('/api/rules')).then(r => r.text()).catch(() => '');
      if (!document.getElementById('rules').matches(':focus')) document.getElementById('rules').value = rules;
    }
    function renderForm(f) {
      const readonly = ['submitted', 'manual_required', 'cancelled'].includes(f.status);
      const fields = (f.fields || []).map((field, i) => `
        <label>${i + 1}. ${esc(field.label || field.id)} ${field.required ? '*' : ''}
          <input id="field-${esc(f.id)}-${i}" value="${esc(field.value || '')}" ${readonly ? 'disabled' : ''}>
        </label>
        ${(field.options || []).length ? `<small>options: ${esc((field.options || []).join(', '))}</small>` : ''}
      `).join('');
      let actions = '';
      if (f.status === 'waiting_user_review') {
        actions = `<button onclick="saveFields('${f.id}')">Save fields</button>
          <button onclick="fillForm('${f.id}')">Fill form</button>
          <button onclick="manualForm('${f.id}')">Manual</button>
          <button onclick="cancelForm('${f.id}')">Cancel</button>`;
      } else if (f.status === 'waiting_submit_confirm') {
        actions = `<button onclick="submitForm('${f.id}')">Submit</button>
          <button onclick="manualForm('${f.id}')">Manual</button>
          <button onclick="cancelForm('${f.id}')">Cancel</button>`;
      } else if (f.status === 'waiting_auth') {
        actions = `<h4>Authorization</h4>
          <input id="user-${f.id}" autocomplete="username" placeholder="login / email">
          <input id="pass-${f.id}" type="password" autocomplete="current-password" placeholder="password">
          <label><input id="remember-${f.id}" type="checkbox" style="width:auto"> remember credentials locally</label>
          <button onclick="authLogin('${f.id}')">Login</button>
          <button onclick="reinspectForm('${f.id}')">Reinspect after manual login</button>
          <button onclick="manualForm('${f.id}')">Manual</button>
          <button onclick="cancelForm('${f.id}')">Cancel</button>`;
      } else if (f.status === 'waiting_2fa') {
        actions = `<h4>Two-factor code</h4>
          <input id="code-${f.id}" autocomplete="one-time-code" inputmode="numeric" placeholder="2FA code">
          <button onclick="submit2fa('${f.id}')">Submit 2FA</button>
          <button onclick="reinspectForm('${f.id}')">Reinspect</button>
          <button onclick="manualForm('${f.id}')">Manual</button>
          <button onclick="cancelForm('${f.id}')">Cancel</button>`;
      } else {
        actions = `<span class="readonly">readonly</span>`;
      }
      return `<div class="form"><b>${esc(f.title || f.form_url)}</b><br>Status: ${esc(f.status)}<br>${esc(f.form_type || '')}<br>${esc(f.form_url)}
        <div>${fields}</div>
        ${actions}
      </div>`;
    }
    async function saveRules() {
      const body = document.getElementById('rules').value;
      try { await api('/api/rules', {method:'POST', body}); document.getElementById('save-result').innerText = 'saved'; }
      catch (e) { document.getElementById('save-result').innerText = 'error: ' + e.message; }
    }
    async function saveProfile() {
      const body = document.getElementById('profile').value;
      try { await api('/api/profile', {method:'POST', body}); document.getElementById('profile-result').innerText = 'saved'; }
      catch (e) { document.getElementById('profile-result').innerText = 'error: ' + e.message; }
    }
    async function saveFields(id) {
      const form = await api(`/api/forms/${id}`);
      const fields = (form.fields || []).map((field, i) => ({id: field.id, value: document.getElementById(`field-${id}-${i}`).value}));
      await api(`/api/forms/${id}/field`, {method:'POST', body: JSON.stringify({fields})});
      await load();
    }
    async function fillForm(id) {
      try { await saveFields(id); await api(`/api/forms/${id}/fill`, {method:'POST'}); showError(''); }
      catch (e) { showError(e); }
      load();
    }
    async function submitForm(id) {
      if (!confirm('Отправить форму?')) return;
      try { await api(`/api/forms/${id}/submit`, {method:'POST'}); showError(''); }
      catch (e) { showError(e); }
      load();
    }
    async function manualForm(id) { try { await api(`/api/forms/${id}/manual`, {method:'POST'}); showError(''); } catch(e) { showError(e); } load(); }
    async function cancelForm(id) { try { await api(`/api/forms/${id}/cancel`, {method:'POST'}); showError(''); } catch(e) { showError(e); } load(); }
    async function reinspectForm(id) { try { await api(`/api/forms/${id}/reinspect`, {method:'POST'}); showError(''); } catch(e) { showError(e); } load(); }
    async function authLogin(id) {
      const username = document.getElementById(`user-${id}`).value;
      const passwordEl = document.getElementById(`pass-${id}`);
      const remember = document.getElementById(`remember-${id}`).checked;
      const password = passwordEl.value;
      passwordEl.value = '';
      try {
        await api(`/api/forms/${id}/auth/credentials`, {method:'POST', body: JSON.stringify({username, password, remember})});
        showError('');
      } catch(e) { showError(e); }
      load();
    }
    async function submit2fa(id) {
      const codeEl = document.getElementById(`code-${id}`);
      const code = codeEl.value;
      codeEl.value = '';
      try { await api(`/api/forms/${id}/auth/2fa`, {method:'POST', body: JSON.stringify({code})}); showError(''); }
      catch(e) { showError(e); }
      load();
    }
    async function testService(name) {
      try {
        const out = await api(`/api/test/${name}`, {method:'POST'});
        document.getElementById('test-result').innerText = JSON.stringify(out);
      } catch(e) {
        document.getElementById('test-result').innerText = 'error: ' + e.message;
      }
    }
    async function createDemo(authDemo) {
      try {
        await api(authDemo ? '/api/demo/create-auth' : '/api/demo/create', {method:'POST'});
        showError('');
      } catch(e) { showError(e); }
      load();
    }
    load();
    setInterval(load, 5000);
  </script>
</body>
</html>
)HTML";

}  // namespace

static void set_json_result(httplib::Response& res, bool ok, const std::string& err, int error_status = 400) {
  res.status = ok ? 200 : error_status;
  res.set_content(nlohmann::json({{"ok", ok}, {"error", ok ? "" : err}}).dump(),
                  "application/json; charset=utf-8");
}

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

    server.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.get_status_json ? handlers.get_status_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
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

    server.Get("/api/rules", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.get_rules_json ? handlers.get_rules_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post("/api/rules", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.set_rules_json ? handlers.set_rules_json(req.body, err) : false;
      set_json_result(res, ok, err.empty() ? "invalid" : err);
    });

    server.Get("/api/forms/active", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      bool all = req.has_param("all") && req.get_param_value("all") == "true";
      std::string body = handlers.get_active_forms_json ? handlers.get_active_forms_json(all) : "[]";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Get(R"(/api/forms/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.get_form_json ? handlers.get_form_json(req.matches[1]) : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post(R"(/api/forms/([^/]+)/field)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.update_form_field_json ?
          handlers.update_form_field_json(req.matches[1], req.body, err) : false;
      set_json_result(res, ok, err.empty() ? "invalid" : err);
    });

    auto post_form_action = [this](const httplib::Request& req,
                                   httplib::Response& res,
                                   const std::function<bool(const std::string&, std::string&)>& fn) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = fn ? fn(req.matches[1], err) : false;
      set_json_result(res, ok, err.empty() ? "failed" : err);
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
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.auth_credentials ? handlers.auth_credentials(req.matches[1], req.body, err) : false;
      set_json_result(res, ok, err);
    });
    server.Post(R"(/api/forms/([^/]+)/auth/2fa)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.auth_two_factor ? handlers.auth_two_factor(req.matches[1], req.body, err) : false;
      set_json_result(res, ok, err);
    });

    server.Get("/api/profile", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.get_profile_json ? handlers.get_profile_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post("/api/profile", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.set_profile_json ? handlers.set_profile_json(req.body, err) : false;
      set_json_result(res, ok, err.empty() ? "invalid" : err);
    });

    server.Get("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.get_config_json ? handlers.get_config_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post("/api/test/browser", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      res.set_content(handlers.test_browser_json ? handlers.test_browser_json() : "{\"ok\":false}",
                      "application/json; charset=utf-8");
    });
    server.Post("/api/test/llm", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      res.set_content(handlers.test_llm_json ? handlers.test_llm_json() : "{\"ok\":false}",
                      "application/json; charset=utf-8");
    });
    server.Post("/api/test/telegram", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      res.set_content(handlers.test_telegram_json ? handlers.test_telegram_json() : "{\"ok\":false}",
                      "application/json; charset=utf-8");
    });

    server.Post("/api/demo/create", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.create_demo_form ? handlers.create_demo_form(false, err) : false;
      set_json_result(res, ok, err.empty() ? "demo creation failed" : err);
    });
    server.Post("/api/demo/create-auth", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.create_demo_form ? handlers.create_demo_form(true, err) : false;
      set_json_result(res, ok, err.empty() ? "demo auth creation failed" : err);
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
  bool auth_ok(const httplib::Request& req, httplib::Response& res) const {
    if (cfg.auth_token.empty()) return true;
    std::string token;
    if (req.has_header("X-Auth-Token")) token = req.get_header_value("X-Auth-Token");
    if (token.empty() && req.has_header("Authorization")) {
      std::string auth = req.get_header_value("Authorization");
      const std::string prefix = "Bearer ";
      if (auth.rfind(prefix, 0) == 0) token = auth.substr(prefix.size());
    }
    if (token.empty() && req.has_param("token")) token = req.get_param_value("token");
    if (token == cfg.auth_token) return true;
    res.status = 401;
    res.set_content("{\"ok\":false,\"error\":\"unauthorized\"}", "application/json; charset=utf-8");
    return false;
  }

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
