#pragma once

#include "../app/Config.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

struct telegram_button {
  std::string text;
  std::string callback_data;
};

struct telegram_update {
  long long update_id = 0;
  std::string chat_id;
  std::string text;
  std::string callback_data;
  std::string callback_query_id;
  long long message_id = 0;
};

class telegram_bot {
public:
  explicit telegram_bot(telegram_config cfg);

  bool enabled() const;
  bool send_message(const std::string& text,
                    const std::vector<std::vector<telegram_button>>& inline_keyboard,
                    std::string& err) const;
  bool send_message(const std::string& text, std::string& err) const;
  std::vector<telegram_update> get_updates(long long offset, std::string& err) const;
  bool answer_callback_query(const std::string& callback_query_id,
                             const std::string& text,
                             std::string& err) const;

private:
  bool request(const std::string& method,
               const nlohmann::json& payload,
               nlohmann::json& response,
               std::string& err) const;

  telegram_config cfg;
};
