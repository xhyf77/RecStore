#pragma once

#include <memory>
#include <string>

#include "base/json.h"
#include "ps/base/base_client.h"

namespace recstore {

std::string BackendNameFromConfig(const json& config);
json load_config_from_file(const std::string& config_path);
json GetGlobalConfig();
void ConfigureLogging(bool initialize_google_logging = true);
std::unique_ptr<BasePSClient> create_ps_client_from_config(const json& config);

} // namespace recstore
