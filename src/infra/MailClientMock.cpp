#include "MailClient.h"

#include <string>
#include <vector>

class mail_client_mock : public mail_client {
public:
  std::uint64_t fetch_max_uid() override {
    return 2;
  }

  std::vector<message> fetch_after_uid(std::uint64_t last_seen_uid) override {
    std::vector<message> res;

    message a;
    a.uid = "1";
    a.mailbox_id = "demo";
    a.provider = "demo";
    a.message_id = "<m1@demo>";
    a.from = "dean@university.edu";
    a.to = "student@example.com";
    a.subject = "Important: scholarship documents";
    a.snippet = "Please submit documents by Friday.";
    a.body = "Please submit documents by Friday: https://forms.yandex.ru/cloud/demo";
    a.body_text = a.body;
    a.date_iso = "2026-02-04";
    a.links.push_back({"https://forms.yandex.ru/cloud/demo", "forms.yandex.ru", 0.95});
    if (last_seen_uid < 1) res.push_back(a);

    message b;
    b.uid = "2";
    b.mailbox_id = "demo";
    b.provider = "demo";
    b.message_id = "<m2@demo>";
    b.from = "news@somewhere.com";
    b.to = "student@example.com";
    b.subject = "Weekly digest";
    b.snippet = "Lots of random updates...";
    b.body = "Lots of random updates...";
    b.body_text = b.body;
    b.date_iso = "2026-02-04";
    if (last_seen_uid < 2) res.push_back(b);

    return res;
  }

  std::vector<message> fetch_last_n(int n) override {
    auto all = fetch_after_uid(0);
    if (n > 0 && static_cast<int>(all.size()) > n) {
      all.erase(all.begin(), all.end() - n);
    }
    return all;
  }
};

mail_client* make_mail_client_mock() {
  return new mail_client_mock();
}
