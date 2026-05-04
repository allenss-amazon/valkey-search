/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/schema_attribute_storage.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "absl/log/check.h"

namespace valkey_search {

namespace {

// Aligns `offset` upward to the given power-of-two `align`.
inline size_t AlignUp(size_t offset, size_t align) {
  return (offset + align - 1) & ~(align - 1);
}

}  // namespace

std::optional<size_t> SchemaAttributeRegistry::SlotOffset(const void* chunk,
                                                          uint32_t slot) const {
  DCHECK(chunk != nullptr);
  DCHECK_LT(slot, SlotCount());

  const uint8_t* bitmap = static_cast<const uint8_t*>(chunk);
  const uint32_t byte_idx = slot / 8;
  const uint32_t bit_idx = slot % 8;
  if ((bitmap[byte_idx] & (uint8_t{1} << bit_idx)) == 0) {
    return std::nullopt;
  }

  size_t offset = BitmapBytes();

  // Walk full bytes before the target byte; skip zero bytes for sparsity.
  for (uint32_t b = 0; b < byte_idx; ++b) {
    uint8_t byte = bitmap[b];
    if (byte == 0) {
      continue;
    }
    while (byte != 0) {
      const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(byte));
      const SlotEntry& e = entries_[b * 8 + bit];
      offset = AlignUp(offset, e.align);
      offset += e.size;
      byte &= static_cast<uint8_t>(byte - 1);
    }
  }
  // Bits in the target byte that come before bit_idx.
  uint8_t last = bitmap[byte_idx] & static_cast<uint8_t>((1u << bit_idx) - 1);
  while (last != 0) {
    const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(last));
    const SlotEntry& e = entries_[byte_idx * 8 + bit];
    offset = AlignUp(offset, e.align);
    offset += e.size;
    last &= static_cast<uint8_t>(last - 1);
  }

  // Align to the target slot's own alignment.
  offset = AlignUp(offset, entries_[slot].align);
  return offset;
}

void SchemaAttributeRegistry::DestroyContents(KeyAttributeData& data) const {
  if (data.chunk_ == nullptr) {
    return;
  }
  uint8_t* bitmap = static_cast<uint8_t*>(data.chunk_);
  const size_t bitmap_bytes = BitmapBytes();
  size_t offset = bitmap_bytes;

  for (size_t b = 0; b < bitmap_bytes; ++b) {
    uint8_t byte = bitmap[b];
    if (byte == 0) {
      continue;
    }
    while (byte != 0) {
      const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(byte));
      const uint32_t k = static_cast<uint32_t>(b) * 8 + bit;
      const SlotEntry& e = entries_[k];
      offset = AlignUp(offset, e.align);
      e.destroy(static_cast<char*>(data.chunk_) + offset);
      offset += e.size;
      byte &= static_cast<uint8_t>(byte - 1);
    }
  }
  ::operator delete(data.chunk_, std::align_val_t{max_align_});
  data.chunk_ = nullptr;
}

KeyAttributeDataBuilder::KeyAttributeDataBuilder(
    KeyAttributeDataBuilder&& other) noexcept
    : registry_(other.registry_), staged_(std::move(other.staged_)) {
  other.staged_.clear();
}

KeyAttributeDataBuilder::~KeyAttributeDataBuilder() {
  for (auto& [slot, staged] : staged_) {
    const auto& e = registry_->Get(slot);
    e.destroy(staged.buffer);
    ::operator delete(staged.buffer, std::align_val_t{e.align});
  }
}

void KeyAttributeDataBuilder::MaterializeInto(KeyAttributeData& out) && {
  DCHECK(out.chunk_ == nullptr)
      << "MaterializeInto target must be empty (call DestroyContents first)";

  if (staged_.empty()) {
    return;
  }

  std::vector<uint32_t> slots;
  slots.reserve(staged_.size());
  for (const auto& [slot, _] : staged_) {
    slots.push_back(slot);
  }
  std::sort(slots.begin(), slots.end());

  const size_t bitmap_bytes = registry_->BitmapBytes();
  const size_t max_align = registry_->MaxAlign();

  // Compute the total chunk size by simulating the layout.
  size_t total = bitmap_bytes;
  for (uint32_t slot : slots) {
    const auto& e = registry_->Get(slot);
    total = AlignUp(total, e.align);
    total += e.size;
  }

  void* chunk = ::operator new(total, std::align_val_t{max_align});
  std::memset(chunk, 0, bitmap_bytes);
  uint8_t* bitmap = static_cast<uint8_t*>(chunk);
  for (uint32_t slot : slots) {
    bitmap[slot / 8] |= static_cast<uint8_t>(uint8_t{1} << (slot % 8));
  }

  // Move-construct each staged payload into its final position, then destroy
  // and free the staging buffer.
  size_t offset = bitmap_bytes;
  for (uint32_t slot : slots) {
    const auto& e = registry_->Get(slot);
    offset = AlignUp(offset, e.align);
    void* dst = static_cast<char*>(chunk) + offset;
    void* src = staged_[slot].buffer;
    e.move_construct(dst, src);
    e.destroy(src);
    ::operator delete(src, std::align_val_t{e.align});
    offset += e.size;
  }
  staged_.clear();
  out.chunk_ = chunk;
}

}  // namespace valkey_search
