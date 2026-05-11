#pragma once

#include "../domain/Message.h"
#include "../infra/Storage.h"
#include "Config.h"

#include <string>


class email_ingestion_service {
public:
  email_ingestion_service(storage& store, const app_config& cfg)
    : store(store), cfg(cfg) {}

  std::string ingest(const message& msg);

private:
  storage& store;
  const app_config& cfg;
};
