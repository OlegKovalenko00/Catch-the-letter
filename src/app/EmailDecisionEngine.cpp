#include "EmailDecisionEngine.h"

static int importance_rank(importance_level level) {
  switch (level) {
    case importance_level::critical: return 4;
    case importance_level::high:     return 3;
    case importance_level::medium:   return 2;
    case importance_level::low:      return 1;
    case importance_level::ignore:   return 0;
  }
  return 0;
}

static importance_level parse_level(const std::string& s) {
  return parse_importance_level(s);
}

bool email_decision_engine::meets_notify_threshold(const email_analysis& analysis) const {
  if (!analysis.should_notify) return false;
  int min_rank = importance_rank(parse_level(cfg.notify_min_importance));
  return importance_rank(analysis.level) >= min_rank;
}

email_decision email_decision_engine::decide(const email_analysis& analysis,
                                              const message& ) const {
  email_decision decision;

  switch (analysis.kind) {
    case message_kind::form_request:
      if (!analysis.form_links.empty() || analysis.contains_form) {
        decision.action = email_action::form_fill;
        decision.reason = "form_link_detected";
      } else if (meets_notify_threshold(analysis)) {
        decision.action = email_action::notify;
        decision.reason = "form_keyword_no_link";
      } else {
        decision.action = email_action::ignore;
        decision.reason = "form_below_threshold";
      }
      break;

    case message_kind::important_notification:
    case message_kind::action_required:
    case message_kind::auth_required:

      if (analysis.level == importance_level::critical ||
          meets_notify_threshold(analysis) ||
          cfg.notify_important_without_rules) {
        decision.action = email_action::notify;
        decision.reason = "important_notification";
      } else {
        decision.action = email_action::ignore;
        decision.reason = "below_notify_threshold";
      }
      break;

    case message_kind::ignored:

      if ((importance_rank(analysis.level) >= importance_rank(importance_level::high) &&
           (analysis.should_notify || analysis.importance_score >= 0.7)) ||
          analysis.level == importance_level::critical) {
        decision.action = email_action::notify;
        decision.reason = "ignored_kind_but_high_importance";
      } else {
        decision.action = email_action::ignore;
        decision.reason = "classified_as_ignored";
      }
      break;

    default:

      if (cfg.classify_unmatched_with_llm && meets_notify_threshold(analysis)) {
        decision.action = email_action::notify;
        decision.reason = "llm_score_above_threshold";
      } else {
        decision.action = email_action::ignore;
        decision.reason = "unknown_kind";
      }
      break;
  }

  return decision;
}
