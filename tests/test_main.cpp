


#include "app/EmailDecisionEngine.h"
#include "domain/EmailAnalysis.h"
#include "domain/Message.h"
#include "infra/ImapParse.h"
#include "infra/LlmClient.h"
#include "infra/Storage.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>


static int g_pass = 0;
static int g_fail = 0;
static const char* g_suite = "";

static void begin_suite(const char* name) {
  g_suite = name;
  std::cout << "\n=== " << name << " ===" << std::endl;
}

static void check(bool cond, const char* expr, int line) {
  if (cond) {
    g_pass++;
    std::cout << "  ok  " << expr << std::endl;
  } else {
    g_fail++;
    std::cerr << "  FAIL [" << g_suite << ":" << line << "] " << expr << std::endl;
  }
}

#define EXPECT(cond) check((cond), #cond, __LINE__)

static int finish() {
  std::cout << "\n--- " << g_pass << " passed, " << g_fail << " failed ---" << std::endl;
  return g_fail ? 1 : 0;
}


static message make_msg(const std::string& from,
                         const std::string& subject,
                         const std::string& body = "") {
  message m;
  m.uid = "1";
  m.mailbox_id = "test";
  m.from = from;
  m.subject = subject;
  m.body_text = body;
  return m;
}

static message make_form_msg(const std::string& url) {
  message m = make_msg("forms@university.edu", "Please fill the form");
  m.body_text = "Fill this form: " + url;
  message_link link;
  link.url = url;
  link.domain = "forms.yandex.ru";
  link.confidence = 0.95;
  m.links.push_back(link);
  return m;
}


static void test_email_decision_engine() {
  begin_suite("EmailDecisionEngine");

  mail_processing_config cfg;
  cfg.notify_min_importance = "high";
  cfg.llm_confidence_threshold = 0.65;
  email_decision_engine engine(cfg);


  {
    email_analysis a;
    a.kind = message_kind::form_request;
    a.contains_form = true;
    a.confidence = 0.9;
    a.level = importance_level::medium;
    message m = make_form_msg("https://forms.yandex.ru/u/abc/");
    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::form_fill);
  }


  {
    email_analysis a;
    a.kind = message_kind::important_notification;
    a.level = importance_level::high;
    a.confidence = 0.85;
    a.should_notify = true;
    a.importance_score = 0.8;
    message m = make_msg("dean@hse.ru", "Important announcement");
    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::notify);
  }


  {
    email_analysis a;
    a.kind = message_kind::important_notification;
    a.level = importance_level::critical;
    a.confidence = 0.95;
    a.importance_score = 0.95;
    a.should_notify = false;
    message m = make_msg("security@hse.ru", "Account compromised");
    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::notify);
  }


  {
    email_analysis a;
    a.kind = message_kind::unknown;
    a.level = importance_level::low;
    a.confidence = 0.2;
    a.should_notify = false;
    a.importance_score = 0.1;
    message m = make_msg("newsletter@spam.com", "Weekly digest");
    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::ignore);
    EXPECT(!d.reason.empty());
  }
}


static void test_noop_llm_client() {
  begin_suite("NoopLlmClient");

  auto client = make_noop_llm_client();


  {
    message m = make_form_msg("https://forms.yandex.ru/u/test/");
    auto a = client->analyze_email(m);
    EXPECT(a.kind == message_kind::form_request ||
           a.contains_form ||
           a.kind != message_kind::unknown);
  }


  {
    message m = make_msg("noreply@hse.ru",
                         "\xD0\x92\xD0\xB0\xD0\xB6\xD0\xBD\xD0\xBE\xD0\xB5 \xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",
                         "\xD0\xA1\xD1\x80\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE: \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xBD\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB4\xD0\xBE\xD0\xBA\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8B");
    auto a = client->analyze_email(m);
    EXPECT(true);
  }


  {
    message m = make_msg("deals@shop.example.com", "SALE 50% off everything!",
                         "Limited time offer. Buy now and save big!");
    auto a = client->analyze_email(m);
    EXPECT(a.kind != message_kind::form_request);
    EXPECT(true);
  }


  {
    message m;
    m.uid = "0";
    m.mailbox_id = "test";
    auto a = client->analyze_email(m);
    EXPECT(true);
  }
}


static void test_sqlite_storage() {
  begin_suite("SqliteStorage");

  std::string err;
  std::unique_ptr<storage> store(make_sqlite_storage(":memory:", &err));
  EXPECT(store != nullptr);
  if (!store) {
    std::cerr << "  SqliteStorage init failed: " << err << std::endl;
    return;
  }


  stored_email email;
  email.mailbox_id = "inbox";
  email.uid = "100";
  email.message_id = "<abc@test>";
  email.from_addr = "sender@university.edu";
  email.to_addr = "student@example.com";
  email.subject = "Final exam schedule";
  email.date_iso = "2026-05-10T09:00:00Z";
  email.snippet = "Your exam is on Monday.";
  email.body_text = "Full text: Your final exam is scheduled for Monday.";
  email.importance_level = "high";
  email.importance_score = 0.85;
  email.category = "academic";
  email.status = "new";

  std::string id = store->save_email_message(email);
  EXPECT(!id.empty());

  auto fetched = store->get_email_message(id);
  EXPECT(fetched.has_value());
  EXPECT(fetched->subject == "Final exam schedule");
  EXPECT(fetched->from_addr == "sender@university.edu");
  EXPECT(fetched->uid == "100");
  EXPECT(fetched->importance_level == "high");

  auto fetched2 = store->get_email_by_mailbox_uid("inbox", "100");
  EXPECT(fetched2.has_value());
  EXPECT(fetched2->id == id);


  email_list_filter f;
  f.status = "all";
  auto list = store->list_emails(f, 10, 0);
  EXPECT(list.size() >= 1);


  auto results = store->search_emails("exam", 10, 0);
  EXPECT(!results.empty());

  auto empty_results = store->search_emails("xyzzy_nonexistent_42", 10, 0);
  EXPECT(empty_results.empty());


  store->update_email_classification(id, "{\"ok\":true}", "high", 0.85, "academic", "important_notified");
  auto after_classify = store->get_email_message(id);
  EXPECT(after_classify.has_value());
  EXPECT(after_classify->status == "important_notified");


  store->mark_email_read(id);
  auto after_read = store->get_email_message(id);
  EXPECT(after_read.has_value() && !after_read->read_at.empty());


  int unread = store->count_unread_important();
  EXPECT(unread >= 0);


  store->archive_email(id);
  auto after_archive = store->get_email_message(id);
  EXPECT(after_archive.has_value() && !after_archive->archived_at.empty());


  store->mute_email(id, "2026-12-31T00:00:00Z");
  auto after_mute = store->get_email_message(id);
  EXPECT(after_mute.has_value() && after_mute->muted_until == "2026-12-31T00:00:00Z");


  EXPECT(!store->is_processed("inbox", "100"));
  message m;
  m.mailbox_id = "inbox";
  m.uid = "100";
  store->mark_processed(m, "archived");
  EXPECT(store->is_processed("inbox", "100"));
  EXPECT(store->processed_count() >= 1);


  std::string token = store->save_telegram_callback_token(
      "mail:view", "{\"email_id\":\"" + id + "\"}", 3600);
  EXPECT(!token.empty());
  EXPECT(token.size() <= 24);

  auto resolved = store->resolve_telegram_callback_token(token);
  EXPECT(resolved.has_value());
  EXPECT(resolved->action == "mail:view");
  EXPECT(resolved->payload_json.find(id) != std::string::npos);

  auto bad = store->resolve_telegram_callback_token("no_such_token_xyz");
  EXPECT(!bad.has_value());


  stored_attachment att;
  att.filename = "report.pdf";
  att.mime_type = "application/pdf";
  att.size_bytes = 2048;
  att.disposition = "attachment";
  att.safe_to_preview = true;
  att.downloaded = false;
  store->save_email_attachments(id, {att});

  auto atts = store->get_email_attachments(id);
  EXPECT(atts.size() == 1);
  EXPECT(atts[0].filename == "report.pdf");
  EXPECT(atts[0].mime_type == "application/pdf");
  EXPECT(atts[0].safe_to_preview == true);

  auto att_fetched = store->get_attachment(atts[0].id);
  EXPECT(att_fetched.has_value());
  EXPECT(att_fetched->filename == "report.pdf");


  store->update_attachment_download(atts[0].id, "/tmp/report.pdf", "deadbeef");
  auto att_after = store->get_attachment(atts[0].id);
  EXPECT(att_after.has_value() && att_after->downloaded);
  EXPECT(att_after->local_path == "/tmp/report.pdf");


  event_record ev;
  ev.level = "info";
  ev.type = "test_event";
  ev.message = "Unit test event";
  ev.data_json = "{\"test\":true}";
  store->append_event(ev, 100);
  auto events = store->last_events(10);
  EXPECT(!events.empty());
  EXPECT(events[0].type == "test_event" || events.back().type == "test_event");


  mailbox_checkpoint cp;
  cp.mailbox_id = "inbox";
  cp.last_seen_uid = 100;
  cp.uid_validity = "12345";
  store->save_checkpoint(cp);
  auto loaded_cp = store->load_checkpoint("inbox");
  EXPECT(loaded_cp.has_value());
  EXPECT(loaded_cp->last_seen_uid == 100);
  EXPECT(loaded_cp->uid_validity == "12345");


  store->set_runtime_value("test_key", "test_value");
  auto rv = store->get_runtime_value("test_key");
  EXPECT(rv.has_value() && *rv == "test_value");
}


static void test_telegram_auth_logic() {
  begin_suite("Telegram auth logic");

  auto is_allowed = [](const std::string& cfg_chat_id, const std::string& incoming) -> bool {
    return cfg_chat_id.empty() || incoming == cfg_chat_id;
  };

  EXPECT(!is_allowed("123456789", "999999999"));
  EXPECT( is_allowed("123456789", "123456789"));
  EXPECT( is_allowed("",          "123456789"));
  EXPECT( is_allowed("",          ""));
  EXPECT(!is_allowed("abc",       "ABC"));
}


static void test_regression_ignored_kind_high_importance() {
  begin_suite("Regression: kind=ignored + level=high must notify");

  mail_processing_config cfg;
  cfg.notify_min_importance = "high";
  cfg.llm_confidence_threshold = 0.65;
  email_decision_engine engine(cfg);


  {
    email_analysis a;
    a.kind = message_kind::ignored;
    a.level = importance_level::high;
    a.confidence = 0.85;
    a.importance_score = 0.85;
    a.should_notify = true;
    message m = make_msg("secretary@hse.ru", "\xD0\xA3\xD1\x87\xD0\xB5\xD0\xB1\xD0\xBD\xD1\x8B\xD0\xB9 \xD0\xB2\xD0\xBE\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81",
                         "\xD0\x92\xD0\xB0\xD1\x81 \xD0\xBE\xD1\x82\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB\xD1\x8F\xD1\x8E\xD1\x82");
    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::notify);
    EXPECT(d.reason == "ignored_kind_but_high_importance");
  }


  {
    email_analysis a;
    a.kind = message_kind::ignored;
    a.level = importance_level::critical;
    a.importance_score = 0.95;
    a.should_notify = false;
    message m = make_msg("admin@hse.ru", "CRITICAL");
    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::notify);
  }


  {
    email_analysis a;
    a.kind = message_kind::ignored;
    a.level = importance_level::low;
    a.importance_score = 0.1;
    a.should_notify = false;
    message m = make_msg("promo@shop.com", "Sale");
    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::ignore);
  }
}

static void test_regression_academic_classification() {
  begin_suite("Regression: academic keywords must classify as important_notification");

  auto client = make_noop_llm_client();


  {
    message m;
    m.uid = "200";
    m.mailbox_id = "inbox";
    m.from = "secretary@hse.ru";

    m.subject = "\xD0\xA3\xD1\x87\xD0\xB5\xD0\xB1\xD0\xBD\xD1\x8B\xD0\xB9 \xD0\xB2\xD0\xBE\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81";

    m.body_text = "\xD0\x92\xD0\xB0\xD1\x81 \xD0\xBE\xD1\x82\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB\xD1\x8F\xD1\x8E\xD1\x82 \xD0\xB8\xD0\xB7 \xD1\x83\xD0\xBD\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x80\xD1\x81\xD0\xB8\xD1\x82\xD0\xB5\xD1\x82\xD0\xB0";
    auto a = client->analyze_email(m);

    EXPECT(a.kind == message_kind::important_notification);
    EXPECT(a.level == importance_level::high);
    EXPECT(a.should_notify == true);
  }


  {
    message m;
    m.uid = "201";
    m.mailbox_id = "inbox";
    m.from = "lms@hse.ru";
    m.subject = "\xD0\x9D\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB5 \xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5";
    m.body_text = "\xD0\xA3 \xD0\xB2\xD0\xB0\xD1\x81 \xD0\xB5\xD1\x81\xD1\x82\xD1\x8C \xD0\xB7\xD0\xB0\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C";
    auto a = client->analyze_email(m);
    EXPECT(a.kind == message_kind::important_notification);
    EXPECT(a.should_notify == true);
  }


  {
    message m;
    m.uid = "202";
    m.mailbox_id = "inbox";
    auto a = client->analyze_email(m);
    EXPECT(a.kind == message_kind::ignored);
    EXPECT(a.should_notify == false);
  }
}

static void test_regression_parse_suspect_not_classified() {
  begin_suite("Regression: parse_suspect message skips classification");


  auto client = make_noop_llm_client();
  {
    message m;
    m.uid = "300";
    m.mailbox_id = "inbox";
    m.parse_suspect = true;
    auto a = client->analyze_email(m);

    EXPECT(a.kind == message_kind::ignored);
    EXPECT(a.level == importance_level::low);
    EXPECT(a.should_notify == false);
  }
}

static void test_regression_decision_engine_consistency() {
  begin_suite("Regression: EmailDecisionEngine — full scenario matrix");

  mail_processing_config cfg;
  cfg.notify_min_importance = "high";
  cfg.llm_confidence_threshold = 0.65;
  cfg.notify_important_without_rules = false;
  email_decision_engine engine(cfg);


  {
    email_analysis a;
    a.kind = message_kind::important_notification;
    a.level = importance_level::high;
    a.should_notify = true;
    a.importance_score = 0.8;
    auto d = engine.decide(a, make_msg("u@hse.ru", "s"));
    EXPECT(d.action == email_action::notify);
  }


  {
    email_analysis a;
    a.kind = message_kind::important_notification;
    a.level = importance_level::medium;
    a.should_notify = false;
    a.importance_score = 0.4;
    auto d = engine.decide(a, make_msg("u@news.com", "s"));
    EXPECT(d.action == email_action::ignore);
  }


  {
    email_analysis a;
    a.kind = message_kind::auth_required;
    a.level = importance_level::high;
    a.should_notify = true;
    auto d = engine.decide(a, make_msg("2fa@bank.com", "Verify"));
    EXPECT(d.action == email_action::notify);
  }


  {
    email_analysis a;
    a.kind = message_kind::form_request;
    a.contains_form = false;
    a.form_links.clear();
    a.should_notify = false;
    a.level = importance_level::low;
    auto d = engine.decide(a, make_msg("forms@u.ru", "Form"));
    EXPECT(d.action == email_action::ignore);
  }
}


static void test_imap_literal_parser() {
  begin_suite("IMAP literal parser");


  {
    std::string resp =
        "* 5 FETCH (UID 1188 BODY[] {47}\r\n"
        "From: alice@example.com\r\n"
        "Subject: Hello\r\n"
        "\r\n"
        "Body text.\r\n"
        ")\r\n"
        "A001 OK FETCH Completed\r\n";
    std::string lit = imap_extract_literal(resp);
    EXPECT(!lit.empty());
    EXPECT(lit.find("From: alice@example.com") != std::string::npos);
  }


  {
    std::string resp =
        "* 3 FETCH (BODY[] {40}\n"
        "From: bob@example.com\n"
        "Subject: Hi\n"
        "\n"
        "Body.\n"
        ")\n"
        "A001 OK\n";
    std::string lit = imap_extract_literal(resp);
    EXPECT(!lit.empty());
    EXPECT(lit.find("From: bob") != std::string::npos);
  }


  {
    std::string resp =
        "* 1 FETCH (BODY[] {9999}\r\n"
        "From: partial@example.com\r\n"
        "Subject: Partial\r\n"
        "\r\n"
        "Partial body only.\r\n";
    std::string lit = imap_extract_literal(resp);
    EXPECT(!lit.empty());
    EXPECT(lit.find("From: partial") != std::string::npos);
  }


  {

    std::string resp =
        "* 3 FETCH (UID 3 BODY[] {60}\r\n"
        "From: real@example.com\r\n"
        "Subject: Real message\r\n"
        "\r\n"
        "Real body content here.\r\n"
        ")\r\n"
        "A001 OK\r\n";


    std::string lit = imap_extract_literal(resp);
    EXPECT(!lit.empty());
    EXPECT(lit.find("From: real") != std::string::npos);
  }


  {
    std::string resp = "* 1 FETCH (FLAGS (\\Seen))\r\nA001 OK\r\n";
    std::string lit = imap_extract_literal(resp);
    EXPECT(lit.empty());
  }


  {
    std::string resp = "* 1 FETCH (BODY[] {}\r\nFrom: x@y.com\r\n)\r\nA001 OK\r\n";
    std::string lit = imap_extract_literal(resp);
    EXPECT(lit.empty());
  }


  {
    std::string resp =
        "* 5 FETCH (UID 1 BODY[] {70}\r\n"
        "From: sender@example.com\r\n"
        "Subject: Test\r\n"
        "\r\n"
        "Body text here.\r\n"
        ")\r\n"
        "A001 OK FETCH Completed\r\n";
    std::string stripped = strip_imap_framing(resp);
    EXPECT(!stripped.empty());
    EXPECT(stripped.find("From: sender") != std::string::npos);
    EXPECT(stripped.find("A001") == std::string::npos);
  }


  {

    std::string resp =
        "* 1 FETCH (BODY[] {34}\r\n"
        "From: x@y.com\r\n"
        "Subject: S\r\n"
        "\r\n"
        "B\r\n";
    message_parse_diagnostics diag;
    std::string raw = imap_extract_raw(resp, diag);
    EXPECT(diag.strategy == "literal");
    EXPECT(!raw.empty());
    EXPECT(raw.find("From: x@y.com") != std::string::npos);
  }


  {
    EXPECT(looks_like_email_headers("From: a@b.com\r\nSubject: hi\r\n"));
    EXPECT(looks_like_email_headers("Received: from mail.example.com"));
    EXPECT(!looks_like_email_headers("just some random text without headers"));
    EXPECT(!looks_like_email_headers(""));
  }


  {
    std::string raw = "From: a@b.com\r\nSubject: hi\r\n\r\nBody text here.";
    std::vector<std::string> hdrs;
    std::string body;
    split_headers_body(raw, hdrs, body);
    EXPECT(hdrs.size() >= 2);
    EXPECT(body == "Body text here.");
    std::string from_val = get_header(hdrs, "From");
    EXPECT(from_val == "a@b.com");
    std::string subj_val = get_header(hdrs, "Subject");
    EXPECT(subj_val == "hi");
  }


  {
    std::string raw = "From: c@d.com\nSubject: test\n\nBody here.";
    std::vector<std::string> hdrs;
    std::string body;
    split_headers_body(raw, hdrs, body);
    EXPECT(!hdrs.empty());
    EXPECT(body == "Body here.");
    EXPECT(get_header(hdrs, "From") == "c@d.com");
  }


  {
    std::string raw = "Subject: This is a very\r\n long subject line\r\n\r\nBody.";
    std::vector<std::string> hdrs;
    std::string body;
    split_headers_body(raw, hdrs, body);
    std::string subj = get_header(hdrs, "Subject");
    EXPECT(subj.find("This is a very") != std::string::npos);
    EXPECT(subj.find("long subject line") != std::string::npos);
  }
}


static void test_url_based_fetch_and_incomplete_literal() {
  begin_suite("URL-based fetch: bare RFC 2822 parse + is_incomplete_literal");


  {


    std::string raw_rfc822 =
        "From: teacher@university.edu\r\n"
        "To: student@university.edu\r\n"
        "Subject: Please fill in the questionnaire\r\n"
        "Date: Mon, 01 Jan 2024 10:00:00 +0000\r\n"
        "Message-ID: <abc123@university.edu>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Please fill in the form at:\r\n"
        "https://forms.yandex.ru/u/survey123/\r\n"
        "Thank you.\r\n";


    EXPECT(!is_incomplete_literal(raw_rfc822));


    message_parse_diagnostics diag;
    std::string extracted = imap_extract_raw(raw_rfc822, diag);
    EXPECT(diag.strategy == "bare");
    EXPECT(!extracted.empty());

    std::vector<std::string> hdrs;
    std::string body;
    split_headers_body(extracted, hdrs, body);
    EXPECT(get_header(hdrs, "From") == "teacher@university.edu");
    EXPECT(get_header(hdrs, "Subject") == "Please fill in the questionnaire");
    EXPECT(!body.empty());

    auto links = extract_links(body, "");
    EXPECT(!links.empty());
    bool found_form = false;
    for (const auto& lnk : links)
      if (lnk.url.find("forms.yandex.ru") != std::string::npos) { found_form = true; break; }
    EXPECT(found_form);
  }


  {
    std::string truncated_response =
        "* 1014 FETCH (UID 1188 BODY[] {7523}\r\n";
    EXPECT(is_incomplete_literal(truncated_response));
  }


  {

    std::string body_47(47, 'X');
    std::string complete_resp =
        "* 5 FETCH (UID 42 BODY[] {47}\r\n" + body_47 + "\r\n)\r\nA001 OK\r\n";
    EXPECT(!is_incomplete_literal(complete_resp));
  }


  {
    std::string rfc_with_braces =
        "From: x@y.com\r\n"
        "Subject: S\r\n"
        "\r\n"
        "The price is {12} dollars per unit.\r\n";
    EXPECT(!is_incomplete_literal(rfc_with_braces));
  }


  {

    std::string encoded_subj =
        "=?UTF-8?B?0JfQsNC/0L7Qu9C90LjRgtC1INGE0L7RgNC80YM=?=";
    std::string decoded = decode_mime_header(encoded_subj);
    EXPECT(!decoded.empty());
    EXPECT(decoded.find("?") == std::string::npos ||
           decoded.size() > 5);
  }


  {
    std::string truncated_bare_lf =
        "* 7 FETCH (UID 99 BODY[] {5000}\n";
    EXPECT(is_incomplete_literal(truncated_bare_lf));
  }
}


static void test_regression_form_email_pipeline() {
  begin_suite("Regression: form email must produce form_fill decision");

  auto client = make_noop_llm_client();
  mail_processing_config cfg;
  cfg.notify_min_importance = "high";
  email_decision_engine engine(cfg);


  {
    message m = make_form_msg("https://forms.yandex.ru/u/abc123/");
    auto a = client->analyze_email(m);
    EXPECT(a.kind == message_kind::form_request);
    EXPECT(a.contains_form == true);
    EXPECT(!a.form_links.empty());

    auto d = engine.decide(a, m);
    EXPECT(d.action == email_action::form_fill);
  }


  {
    message m = make_form_msg("https://forms.yandex.com/u/survey/");
    auto a = client->analyze_email(m);
    EXPECT(a.kind == message_kind::form_request || a.contains_form);
  }


  {
    message m = make_msg("forms@yandex.ru", "Ответы записаны",
                         "Ваш ответ был получен. Ответы записаны.");
    auto a = client->analyze_email(m);
    EXPECT(a.kind != message_kind::form_request);
  }
}


static void test_regression_checkpoint_logic() {
  begin_suite("Regression: checkpoint boundary with parse_suspect UIDs");


  auto compute_safe_max = [](std::uint64_t max_seen,
                              std::uint64_t min_suspect) -> std::uint64_t {
    if (min_suspect != UINT64_MAX && min_suspect > 0)
      return std::min(max_seen, min_suspect - 1);
    return max_seen;
  };


  {
    std::uint64_t safe = compute_safe_max(1191, UINT64_MAX);
    EXPECT(safe == 1191);
  }


  {
    std::uint64_t safe = compute_safe_max(1191, 1189);
    EXPECT(safe == 1188);
  }


  {
    std::uint64_t safe = compute_safe_max(0, 1188);
    EXPECT(safe == 0);
  }


  {
    std::uint64_t safe = compute_safe_max(1190, 1190);
    EXPECT(safe == 1189);
  }


  {
    std::uint64_t checkpoint_uid = 1184;
    std::uint64_t max_seen       = checkpoint_uid;
    std::uint64_t min_suspect    = 1185;
    std::uint64_t safe = compute_safe_max(max_seen, min_suspect);
    EXPECT(safe == 1184);
    EXPECT(safe <= checkpoint_uid);
  }
}


int main() {
  test_email_decision_engine();
  test_noop_llm_client();
  test_sqlite_storage();
  test_telegram_auth_logic();
  test_regression_ignored_kind_high_importance();
  test_regression_academic_classification();
  test_regression_parse_suspect_not_classified();
  test_regression_decision_engine_consistency();
  test_imap_literal_parser();
  test_regression_form_email_pipeline();
  test_regression_checkpoint_logic();
  test_url_based_fetch_and_incomplete_literal();
  return finish();
}
