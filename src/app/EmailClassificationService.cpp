#include "EmailClassificationService.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

email_analysis email_classification_service::classify(const std::string& email_id,
                                                       const message& msg) {

  if (msg.parse_suspect) {
    email_analysis analysis;
    analysis.kind = message_kind::unknown;
    analysis.level = importance_level::low;
    analysis.confidence = 0.0;
    analysis.importance_score = 0.0;
    analysis.should_notify = false;
    analysis.reasons.push_back("parse_suspect_skipped_classification");
    store.update_email_classification(
        email_id, "{\"parse_suspect\":true}", "low", 0.0, "other", "parse_failed");
    return analysis;
  }

  email_analysis analysis = classifier.analyze_email(msg);


  json cls;
  cls["kind"]                = to_string(analysis.kind);
  cls["confidence"]          = analysis.confidence;
  cls["importance_score"]    = analysis.importance_score;
  cls["level"]               = to_string(analysis.level);
  cls["category"]            = to_string(analysis.category);
  cls["urgency"]             = to_string(analysis.urgency);
  cls["summary"]             = analysis.summary;
  cls["safe_preview"]        = analysis.safe_preview;
  cls["user_action_required"] = analysis.user_action_required;
  cls["should_notify"]       = analysis.should_notify;
  cls["contains_form"]       = analysis.contains_form;
  cls["deadline_text"]       = analysis.deadline_text;
  if (!analysis.reasons.empty()) cls["reasons"] = analysis.reasons;
  if (!analysis.suggested_actions.empty()) cls["suggested_actions"] = analysis.suggested_actions;


  std::string status;
  switch (analysis.kind) {
    case message_kind::form_request:           status = "new";      break;
    case message_kind::important_notification: status = "new";      break;
    case message_kind::action_required:        status = "new";      break;
    case message_kind::auth_required:          status = "new";      break;
    case message_kind::ignored:                status = "ignored";  break;
    default:                                   status = "new";      break;
  }


  if (analysis.confidence < cfg.llm_confidence_threshold &&
      analysis.kind != message_kind::form_request) {
    if (!cfg.fallback_keyword_importance) {
      status = "ignored";
    }
  }

  store.update_email_classification(
      email_id,
      cls.dump(),
      to_string(analysis.level),
      analysis.importance_score,
      to_string(analysis.category),
      status);

  return analysis;
}
