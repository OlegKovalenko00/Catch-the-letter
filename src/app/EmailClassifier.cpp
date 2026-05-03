#include "EmailClassifier.h"

email_analysis email_classifier::analyze_email(const message& msg) {
  return llm.analyze_email(msg);
}
