#pragma once

#include "../domain/EmailAnalysis.h"
#include "../infra/LlmClient.h"

class email_classifier {
public:
  explicit email_classifier(llm_client& llm) : llm(llm) {}
  email_analysis analyze_email(const message& msg);

private:
  llm_client& llm;
};
