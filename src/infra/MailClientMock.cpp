#include "MailClient.h"

#include <string>
#include <vector>

class mail_client_mock : public mail_client {
public:
  std::vector<message> fetch_unseen() override {
    std::vector<message> res;

    message a;
    a.uid = "m1";
    a.message_id = "<m1@demo>";
    a.from = "dean@university.edu";
    a.to = "student@example.com";
    a.subject = "Important: scholarship documents";
    a.snippet = "Please submit documents by Friday.";
    a.body = "Please submit documents by Friday.";
    a.date_iso = "2026-02-04";
    res.push_back(a);

    message b;
    b.uid = "m2";
    b.message_id = "<m2@demo>";
    b.from = "news@somewhere.com";
    b.to = "student@example.com";
    b.subject = "Weekly digest";
    b.snippet = "Lots of random updates...";
    b.body = "Lots of random updates...";
    b.date_iso = "2026-02-04";
    res.push_back(b);

    return res;
  }
};

mail_client* make_mail_client_mock() {
  return new mail_client_mock();
}
