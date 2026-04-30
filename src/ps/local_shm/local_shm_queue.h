#pragma once

#include <cstdint>

#include "ps/local_shm/local_shm_protocol.h"

namespace recstore {

void LocalShmQueueInitialize(
    LocalShmQueueHeader* header, LocalShmQueueCell* cells, uint32_t capacity);

bool LocalShmQueueEnqueue(
    LocalShmQueueHeader* header, LocalShmQueueCell* cells, uint32_t value);

bool LocalShmQueueDequeue(
    LocalShmQueueHeader* header, LocalShmQueueCell* cells, uint32_t* value);

} // namespace recstore
