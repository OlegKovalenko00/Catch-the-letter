#include "TelegramNotifier.h"

#include <iostream>
#include <string>

class telegram_notifier_mock : public telegram_notifier {
public:
  bool notify(const message& msg, const std::string& text, std::string& err) override {
    std::cout << "[TELEGRAM MOCK]" << std::endl;
    std::cout << "subject: " << msg.subject << std::endl;
    std::cout << "text: " << text << std::endl;
    err.clear();
    return true;
  }
};

telegram_notifier* make_telegram_notifier_mock() {
  return new telegram_notifier_mock();
}
