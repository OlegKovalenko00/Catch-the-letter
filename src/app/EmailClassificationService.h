#pragma once

#include "../domain/EmailAnalysis.h"
#include "../domain/Message.h"
#include "../infra/Storage.h"
#include "Config.h"
#include "EmailClassifier.h"

#include <string>


class email_classification_service {
public:
  email_classification_service(email_classifier& classifier,
                                storage& store,
                                const mail_processing_config& cfg)
    : classifier(classifier), store(store), cfg(cfg) {}

  email_analysis classify(const std::string& email_id, const message& msg);

private:
  email_classifier& classifier;
  storage& store;
  const mail_processing_config& cfg;
};
