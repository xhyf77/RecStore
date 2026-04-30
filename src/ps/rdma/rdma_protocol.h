#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "base/log.h"
#include "third_party/Mayfly-main/include/GlobalAddress.h"

namespace petps {

inline constexpr std::uint32_t kPutPayloadMagic       = 0x50545053;
inline constexpr std::uint16_t kPutProtocolVersionV1  = 1;
inline constexpr std::uint16_t kPutProtocolVersionV2  = 2;
inline constexpr std::uint32_t kPutRemotePayloadMagic = 0x50545232;
inline constexpr std::uint32_t kPutV2TransferModeRead = 0;
inline constexpr std::uint32_t kPutV2TransferModePush = 1;

struct PutPayloadHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint32_t key_count;
  std::uint32_t embedding_dim;
};

struct DecodedPutPayload {
  std::uint32_t embedding_dim = 0;
  std::vector<std::uint64_t> keys;
  std::vector<float> values;
};

struct PutRemotePayloadV2 {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint32_t key_count;
  std::uint32_t embedding_dim;
  GlobalAddress payload_gaddr;
  std::uint32_t payload_bytes;
  std::uint32_t transfer_mode;
  std::uint32_t checksum;
} __attribute__((packed));

static_assert(sizeof(PutRemotePayloadV2) <= 64, "unexpected v2 control size");

inline std::size_t
FixedSlotResponseBytes(std::size_t key_count, std::size_t value_size_bytes) {
  return key_count * value_size_bytes + sizeof(std::int32_t);
}

inline std::size_t
PutPayloadBytes(std::size_t key_count, std::size_t value_size_bytes) {
  return key_count * (sizeof(std::uint64_t) + value_size_bytes);
}

inline std::string
EncodePutPayload(const std::vector<std::uint64_t>& keys,
                 const std::vector<std::vector<float>>& values) {
  if (keys.empty()) {
    return {};
  }

  // Validate keys and values have the same count
  if (keys.size() != values.size()) {
    LOG(ERROR) << "EncodePutPayload: keys.size()=" << keys.size()
               << " != values.size()=" << values.size();
    return {};
  }

  const std::size_t embedding_dim = values.front().size();
  // Validate all values have the same dimension
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (values[i].size() != embedding_dim) {
      LOG(ERROR) << "EncodePutPayload: values[" << i
                 << "].size()=" << values[i].size()
                 << " != expected embedding_dim=" << embedding_dim;
      return {};
    }
  }

  PutPayloadHeader header{
      kPutPayloadMagic,
      kPutProtocolVersionV1,
      0,
      static_cast<std::uint32_t>(keys.size()),
      static_cast<std::uint32_t>(embedding_dim),
  };

  std::string payload;
  payload.resize(
      sizeof(PutPayloadHeader) + keys.size() * sizeof(std::uint64_t) +
      keys.size() * embedding_dim * sizeof(float));

  char* cursor = payload.data();
  std::memcpy(cursor, &header, sizeof(header));
  cursor += sizeof(header);

  std::memcpy(cursor, keys.data(), keys.size() * sizeof(std::uint64_t));
  cursor += keys.size() * sizeof(std::uint64_t);

  for (const auto& row : values) {
    std::memcpy(cursor, row.data(), embedding_dim * sizeof(float));
    cursor += embedding_dim * sizeof(float);
  }

  return payload;
}

inline bool DecodePutPayload(
    std::string_view payload, DecodedPutPayload* decoded, std::string* error) {
  if (payload.size() < sizeof(PutPayloadHeader)) {
    if (error != nullptr) {
      *error = "payload smaller than header";
    }
    return false;
  }

  PutPayloadHeader header{};
  std::memcpy(&header, payload.data(), sizeof(header));

  if (header.magic != kPutPayloadMagic) {
    if (error != nullptr) {
      *error = "bad payload magic";
    }
    return false;
  }
  if (header.version != kPutProtocolVersionV1) {
    if (error != nullptr) {
      *error = "bad protocol version";
    }
    return false;
  }

  const std::size_t expected_bytes =
      sizeof(PutPayloadHeader) + header.key_count * sizeof(std::uint64_t) +
      static_cast<std::size_t>(header.key_count) * header.embedding_dim *
          sizeof(float);

  if (payload.size() != expected_bytes) {
    if (error != nullptr) {
      *error = "payload byte size mismatch";
    }
    return false;
  }

  decoded->embedding_dim = header.embedding_dim;
  decoded->keys.resize(header.key_count);
  decoded->values.resize(
      static_cast<std::size_t>(header.key_count) * header.embedding_dim);

  const char* cursor = payload.data() + sizeof(PutPayloadHeader);
  std::memcpy(
      decoded->keys.data(), cursor, header.key_count * sizeof(std::uint64_t));
  cursor += header.key_count * sizeof(std::uint64_t);
  std::memcpy(
      decoded->values.data(), cursor, decoded->values.size() * sizeof(float));

  return true;
}

inline bool EncodePutRemoteControlV2(const PutRemotePayloadV2& control,
                                     std::string* payload,
                                     std::string* error) {
  if (payload == nullptr) {
    if (error != nullptr) {
      *error = "payload is null";
    }
    return false;
  }
  if (control.magic != kPutRemotePayloadMagic) {
    if (error != nullptr) {
      *error = "bad control magic";
    }
    return false;
  }
  if (control.version != kPutProtocolVersionV2) {
    if (error != nullptr) {
      *error = "bad control version";
    }
    return false;
  }
  const std::size_t expected_bytes = PutPayloadBytes(
      control.key_count,
      static_cast<std::size_t>(control.embedding_dim) * sizeof(float));
  if (expected_bytes != control.payload_bytes) {
    if (error != nullptr) {
      *error = "control payload_bytes mismatch";
    }
    return false;
  }
  if (control.transfer_mode != kPutV2TransferModeRead &&
      control.transfer_mode != kPutV2TransferModePush) {
    if (error != nullptr) {
      *error = "bad transfer mode";
    }
    return false;
  }
  payload->resize(sizeof(PutRemotePayloadV2));
  std::memcpy(payload->data(), &control, sizeof(PutRemotePayloadV2));
  return true;
}

inline bool DecodePutRemoteControlV2(
    std::string_view payload, PutRemotePayloadV2* control, std::string* error) {
  if (control == nullptr) {
    if (error != nullptr) {
      *error = "control is null";
    }
    return false;
  }
  if (payload.size() != sizeof(PutRemotePayloadV2)) {
    if (error != nullptr) {
      *error = "control size mismatch";
    }
    return false;
  }
  PutRemotePayloadV2 decoded{};
  std::memcpy(&decoded, payload.data(), sizeof(decoded));
  if (decoded.magic != kPutRemotePayloadMagic) {
    if (error != nullptr) {
      *error = "bad control magic";
    }
    return false;
  }
  if (decoded.version != kPutProtocolVersionV2) {
    if (error != nullptr) {
      *error = "bad control version";
    }
    return false;
  }
  const std::size_t expected_bytes = PutPayloadBytes(
      decoded.key_count,
      static_cast<std::size_t>(decoded.embedding_dim) * sizeof(float));
  if (expected_bytes != decoded.payload_bytes) {
    if (error != nullptr) {
      *error = "control payload_bytes mismatch";
    }
    return false;
  }
  if (decoded.transfer_mode != kPutV2TransferModeRead &&
      decoded.transfer_mode != kPutV2TransferModePush) {
    if (error != nullptr) {
      *error = "bad transfer mode";
    }
    return false;
  }
  *control = decoded;
  return true;
}

inline bool IsPutRemoteControlV2(std::string_view payload) {
  if (payload.size() != sizeof(PutRemotePayloadV2)) {
    return false;
  }
  PutRemotePayloadV2 decoded{};
  std::memcpy(&decoded, payload.data(), sizeof(decoded));
  return decoded.magic == kPutRemotePayloadMagic &&
         decoded.version == kPutProtocolVersionV2;
}

} // namespace petps
