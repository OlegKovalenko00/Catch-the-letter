#include "HttpServer.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

const char* k_fallback_html = R"HTML(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Catch the Letter</title>
  <style>
    body { margin: 0; font-family: system-ui, -apple-system, Segoe UI, sans-serif; background: #f7f8fb; color: #172033; }
    main { max-width: 760px; margin: 10vh auto; padding: 28px; background: #fff; border: 1px solid #d8dee9; border-radius: 10px; }
    code { background: #eef2f7; padding: 2px 5px; border-radius: 4px; }
  </style>
</head>
<body>
  <main>
    <h1>Catch the Letter</h1>
    <p>Static Web UI files were not found.</p>
    <p>The server tried <code>/app/web/index.html</code> and <code>./web/index.html</code>.</p>
    <p>In Docker, make sure the runtime image copies <code>web/</code> to <code>/app/web</code>.</p>
  </main>
</body>
</html>
)HTML";

std::optional<std::string> read_file_if_exists(const std::vector<std::string>& paths) {
  for (const auto& path : paths) {
    std::ifstream input(path, std::ios::binary);
    if (!input) continue;
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  return std::nullopt;
}

void set_static_content(httplib::Response& res,
                        const std::vector<std::string>& paths,
                        const std::string& content_type,
                        const char* fallback = nullptr) {
  auto content = read_file_if_exists(paths);
  if (content) {
    res.set_content(*content, content_type);
    return;
  }
  if (fallback != nullptr) {
    res.set_content(fallback, content_type);
    return;
  }
  res.status = 404;
  res.set_content("Not found", "text/plain; charset=utf-8");
}

void set_json_result(httplib::Response& res, bool ok, const std::string& err, int error_status = 400) {
  res.status = ok ? 200 : error_status;
  nlohmann::json body;
  body["ok"] = ok;
  if (ok) {
    body["message"] = "ok";
    body["data"] = nlohmann::json::object();
  } else {
    body["error"] = err;
    body["details"] = nlohmann::json::object();
  }
  res.set_content(body.dump(), "application/json; charset=utf-8");
}

}

class http_server_impl final : public http_server {
public:
  http_server_impl(http_config cfg, http_handlers handlers)
    : cfg(std::move(cfg)), handlers(std::move(handlers)) {}

  bool start() override {
    if (running) return true;
    if (!cfg.enabled) return true;

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
      set_static_content(res,
                         {"/app/web/index.html", "./web/index.html"},
                         "text/html; charset=utf-8",
                         k_fallback_html);
    });
    server.Get("/app.js", [](const httplib::Request&, httplib::Response& res) {
      set_static_content(res,
                         {"/app/web/app.js", "./web/app.js"},
                         "application/javascript; charset=utf-8");
    });
    server.Get("/styles.css", [](const httplib::Request&, httplib::Response& res) {
      set_static_content(res,
                         {"/app/web/styles.css", "./web/styles.css"},
                         "text/css; charset=utf-8");
    });

    server.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.get_status_json ? handlers.get_status_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Get("/api/dashboard", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      if (handlers.get_dashboard_json) {
        res.set_content(handlers.get_dashboard_json(), "application/json; charset=utf-8");
        return;
      }
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
    server.Post("/api/forms/inspect-url", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.inspect_form_url_json ? handlers.inspect_form_url_json(req.body) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"details", nlohmann::json::object()}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });
    server.Post("/api/forms/create-from-url", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.create_form_session_from_url_json ?
          handlers.create_form_session_from_url_json(req.body) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"details", nlohmann::json::object()}}).dump();
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
    server.Post(R"(/api/forms/([^/]+)/fields)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.update_form_field_json ?
          handlers.update_form_field_json(req.matches[1], req.body, err) : false;
      set_json_result(res, ok, err.empty() ? "invalid" : err);
    });
    server.Post(R"(/api/forms/([^/]+)/remap)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.remap_form_json ? handlers.remap_form_json(req.matches[1], req.body) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"details", nlohmann::json::object()}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });
    server.Post(R"(/api/forms/([^/]+)/explain-field)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.explain_form_field_json ? handlers.explain_form_field_json(req.matches[1], req.body) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"details", nlohmann::json::object()}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });
    server.Post(R"(/api/forms/([^/]+)/validate)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.validate_form_json ? handlers.validate_form_json(req.matches[1]) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"details", nlohmann::json::object()}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
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
    server.Post("/api/test/imap", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      res.set_content(handlers.test_imap_json ? handlers.test_imap_json() : "{\"ok\":false}",
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
    server.Post("/api/demo/create-captcha", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.create_demo_captcha_form ? handlers.create_demo_captcha_form(err) : false;
      set_json_result(res, ok, err.empty() ? "demo captcha creation failed" : err);
    });

    server.Get(R"(/api/forms/([^/]+)/screenshot)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string png = handlers.get_form_screenshot_png ? handlers.get_form_screenshot_png(req.matches[1]) : "";
      if (png.empty()) {
        res.status = 404;
        res.set_content("screenshot unavailable", "text/plain; charset=utf-8");
        return;
      }
      res.set_content(png, "image/png");
    });
    server.Post(R"(/api/forms/([^/]+)/captcha-click)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.captcha_click_form ?
          handlers.captcha_click_form(req.matches[1], req.body, err) : false;
      set_json_result(res, ok, err.empty() ? "click failed" : err);
    });
    server.Post(R"(/api/forms/([^/]+)/captcha-reinspect)", [this, post_form_action](const httplib::Request& req, httplib::Response& res) {
      post_form_action(req, res, handlers.captcha_reinspect_form);
    });

    server.Get("/api/debug/mail/status", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.mail_debug_json ? handlers.mail_debug_json() : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });
    server.Post("/api/debug/mail/scan-last", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      int n = 10;
      try {
        auto parsed = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
        n = parsed.value("n", 10);
      } catch (...) {}
      std::string body = handlers.mail_scan_last_json ? handlers.mail_scan_last_json(n) : "{}";
      res.set_content(body, "application/json; charset=utf-8");
    });
    server.Post("/api/debug/mail/reset-state", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string mailbox_id;
      try {
        auto parsed = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
        mailbox_id = parsed.value("mailbox_id", "");
      } catch (...) {}
      std::string body = handlers.mail_reset_state_json ?
          handlers.mail_reset_state_json(mailbox_id) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });

    server.Post("/api/profile/expand-preview", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.expand_profile_preview_json ?
          handlers.expand_profile_preview_json(req.body) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"suggestions", nlohmann::json::array()}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });
    server.Post("/api/profile/apply-expansion", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.apply_profile_expansion_json ?
          handlers.apply_profile_expansion_json(req.body, err) : false;
      set_json_result(res, ok, err.empty() ? "apply failed" : err);
    });


    server.Get("/api/mail", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string filter = req.has_param("filter") ? req.get_param_value("filter") : "all";
      int limit = 20, offset = 0;
      try { if (req.has_param("limit"))  limit  = std::stoi(req.get_param_value("limit"));  } catch (...) {}
      try { if (req.has_param("offset")) offset = std::stoi(req.get_param_value("offset")); } catch (...) {}
      std::string body = handlers.mail_list_json ?
          handlers.mail_list_json(filter, limit, offset) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"emails", nlohmann::json::array()}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });


    server.Get("/api/mail/search", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string q = req.has_param("q") ? req.get_param_value("q") : "";
      int limit = 20, offset = 0;
      try { if (req.has_param("limit"))  limit  = std::stoi(req.get_param_value("limit"));  } catch (...) {}
      try { if (req.has_param("offset")) offset = std::stoi(req.get_param_value("offset")); } catch (...) {}
      std::string body = handlers.mail_search_json ?
          handlers.mail_search_json(q, limit, offset) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}, {"emails", nlohmann::json::array()}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });


    server.Get(R"(/api/mail/attachments/([^/]+)/download)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string content, content_type, filename, err;
      bool ok = handlers.mail_attachment_download &&
                handlers.mail_attachment_download(req.matches[1], content, content_type, filename, err);
      if (!ok) {
        res.status = err.find("not found") != std::string::npos ? 404 : 400;
        res.set_content(
            nlohmann::json({{"ok", false}, {"error", err.empty() ? "attachment unavailable" : err}}).dump(),
            "application/json; charset=utf-8");
        return;
      }
      if (!filename.empty()) {
        std::string safe = filename;
        for (char& c : safe) if (c == '"' || c == '\r' || c == '\n') c = '_';
        res.set_header("Content-Disposition", "attachment; filename=\"" + safe + "\"");
      }
      res.set_content(content, content_type.empty() ? "application/octet-stream" : content_type);
    });


    server.Get(R"(/api/mail/([^/]+)/attachments)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.mail_attachments_json ?
          handlers.mail_attachments_json(req.matches[1]) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });


    server.Post(R"(/api/mail/([^/]+)/read)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.mail_mark_read && handlers.mail_mark_read(req.matches[1], err);
      set_json_result(res, ok, err.empty() ? "read failed" : err);
    });


    server.Post(R"(/api/mail/([^/]+)/archive)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.mail_archive && handlers.mail_archive(req.matches[1], err);
      set_json_result(res, ok, err.empty() ? "archive failed" : err);
    });


    server.Post(R"(/api/mail/([^/]+)/mute)", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string err;
      bool ok = handlers.mail_mute && handlers.mail_mute(req.matches[1], req.body, err);
      set_json_result(res, ok, err.empty() ? "mute failed" : err);
    });


    server.Get(R"(/api/mail/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth_ok(req, res)) return;
      std::string body = handlers.mail_get_json ?
          handlers.mail_get_json(req.matches[1]) :
          nlohmann::json({{"ok", false}, {"error", "handler unavailable"}}).dump();
      res.set_content(body, "application/json; charset=utf-8");
    });


    server.Get(R"(/forms/.*)", [](const httplib::Request&, httplib::Response& res) {
      set_static_content(res,
                         {"/app/web/index.html", "./web/index.html"},
                         "text/html; charset=utf-8",
                         k_fallback_html);
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
    if (token == cfg.auth_token) return true;
    res.status = 401;
    res.set_content("{\"ok\":false,\"error\":\"unauthorized\",\"details\":{}}", "application/json; charset=utf-8");
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
