#pragma once

#include <memory>

#include "ps/base/base_client.h"
#include "ps/client_options.h"

namespace recstore {

std::unique_ptr<BasePSClient>
CreatePSClient(const PSClientCreateOptions& options);

} // namespace recstore
