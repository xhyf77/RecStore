#include "ps/local_shm/local_shm_queue.h"

#include <cstring>
#include <cstdint>

namespace recstore {

void LocalShmQueueInitialize(
    LocalShmQueueHeader* header, LocalShmQueueCell* cells, uint32_t capacity) {
  header->enqueue_pos.store(0, std::memory_order_relaxed);
  header->dequeue_pos.store(0, std::memory_order_relaxed);
  header->capacity  = capacity;
  header->reserved0 = 0;
  std::memset(header->reserved, 0, sizeof(header->reserved));
  for (uint32_t i = 0; i < capacity; ++i) {
    cells[i].sequence.store(i, std::memory_order_relaxed);
    cells[i].value     = 0;
    cells[i].reserved0 = 0;
  }
}

bool LocalShmQueueEnqueue(
    LocalShmQueueHeader* header, LocalShmQueueCell* cells, uint32_t value) {
  const uint32_t capacity = header->capacity;
  uint32_t pos            = header->enqueue_pos.load(std::memory_order_relaxed);
  while (true) {
    LocalShmQueueCell* cell = &cells[pos % capacity];
    const uint32_t seq      = cell->sequence.load(std::memory_order_acquire);
    const intptr_t dif =
        static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
    if (dif == 0) {
      if (header->enqueue_pos.compare_exchange_weak(
              pos, pos + 1, std::memory_order_relaxed)) {
        cell->value = value;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
      }
    } else if (dif < 0) {
      return false;
    } else {
      pos = header->enqueue_pos.load(std::memory_order_relaxed);
    }
  }
}

bool LocalShmQueueDequeue(
    LocalShmQueueHeader* header, LocalShmQueueCell* cells, uint32_t* value) {
  const uint32_t capacity = header->capacity;
  uint32_t pos            = header->dequeue_pos.load(std::memory_order_relaxed);
  while (true) {
    LocalShmQueueCell* cell = &cells[pos % capacity];
    const uint32_t seq      = cell->sequence.load(std::memory_order_acquire);
    const intptr_t dif =
        static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
    if (dif == 0) {
      if (header->dequeue_pos.compare_exchange_weak(
              pos, pos + 1, std::memory_order_relaxed)) {
        *value = cell->value;
        cell->sequence.store(pos + capacity, std::memory_order_release);
        return true;
      }
    } else if (dif < 0) {
      return false;
    } else {
      pos = header->dequeue_pos.load(std::memory_order_relaxed);
    }
  }
}

} // namespace recstore
