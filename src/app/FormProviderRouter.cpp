#include "FormProviderRouter.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace {

std::string lower_ascii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

form_provider_router::form_provider_router(app_config cfg) : cfg(std::move(cfg)) {}

form_provider_type form_provider_router::detect_provider(const std::string& url) const {
  const std::string lower = lower_ascii(url);
  if (contains(lower, "forms.yandex.ru")) {
    // Exclude admin/results pages — these are confirmation URLs, not fillable forms.
    if (contains(lower, "/admin/") || contains(lower, "/answers/") ||
        contains(lower, "/results/") || contains(lower, "/success")) {
      return form_provider_type::generic_browser;
    }
    return form_provider_type::yandex_forms;
  }
  if (contains(lower, "docs.google.com/forms") ||
      contains(lower, "forms.gle") ||
      contains(lower, "google.com/forms") ||
      contains(lower, "/forms/d/e/") ||
      contains(lower, "/forms/d/")) {
    // Exclude Google Forms analytics/response viewer pages.
    if (contains(lower, "/viewanalytics") || contains(lower, "/closedform")) {
      return form_provider_type::generic_browser;
    }
    return form_provider_type::google_forms;
  }
  return form_provider_type::generic_browser;
}

provider_route form_provider_router::route_for_url(const std::string& url) const {
  provider_route route;
  route.provider_type = detect_provider(url);
  route.provider_name = provider_display_name(route.provider_type);
  route.known_provider = route.provider_type != form_provider_type::generic_browser;
  route.allow_browser_fallback = true;

  if (route.provider_type == form_provider_type::yandex_forms) {
    route.submit_strategy = form_submit_strategy::yandex_forms_api;
    // forms.yandex.ru/u/<id> are public user-published forms — allow browser fallback even
    // without API credentials, since they don't require organisation access.
    bool is_public_user_form = lower_ascii(url).find("/u/") != std::string::npos;
    route.allow_browser_fallback =
        is_public_user_form ||
        cfg.form_providers.browser_fallback_for_known_providers ||
        cfg.yandex_forms_api.allow_browser_fallback;
    return route;
  }
  if (route.provider_type == form_provider_type::google_forms) {
    route.submit_strategy = form_submit_strategy::google_forms_api;
    route.allow_browser_fallback =
        cfg.form_providers.browser_fallback_for_known_providers ||
        cfg.google_forms_api.allow_browser_fallback;
    return route;
  }

  route.submit_strategy = form_submit_strategy::browser_worker;
  return route;
}

std::string form_provider_router::provider_type_name(form_provider_type type) {
  return to_string(type);
}

std::string form_provider_router::provider_display_name(form_provider_type type) {
  switch (type) {
    case form_provider_type::yandex_forms: return "Yandex Forms";
    case form_provider_type::google_forms: return "Google Forms";
    case form_provider_type::generic_browser: return "Generic Browser";
  }
  return "Generic Browser";
}

std::string form_provider_router::submit_strategy_name(form_submit_strategy strategy) {
  return to_string(strategy);
}
