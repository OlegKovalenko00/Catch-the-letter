#pragma once

#include "../app/Config.h"
#include "../app/FormProviderRouter.h"
#include "../domain/Form.h"

#include <string>

class yandex_forms_provider {
public:
  explicit yandex_forms_provider(yandex_forms_api_config cfg);

  provider_inspect_result inspect(const std::string& url) const;
  provider_submit_result submit(const form_session& session) const;
  bool can_handle(const std::string& url) const;
  std::string extract_public_form_id(const std::string& url) const;

private:
  yandex_forms_api_config cfg;
};

