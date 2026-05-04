/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEYSEARCH_SRC_SCHEMA_ATTRIBUTE_STORAGE_H_
#define VALKEYSEARCH_SRC_SCHEMA_ATTRIBUTE_STORAGE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"

namespace valkey_search {

// Forward declarations.
template <typename T>
class AttributeSlot;
class SchemaAttributeRegistry;
class KeyAttributeData;
class KeyAttributeDataBuilder;

// Per-key compact holder. Exactly one pointer.
//
// Move-only: moving transfers chunk ownership to the destination and leaves
// the source empty. The chunk's payload objects do not move when the holder
// is moved — only the owning pointer changes.
//
// The destructor does NOT free the chunk. Owners must call
// SchemaAttributeRegistry::DestroyContents(data) before the holder is
// destroyed; the destructor only DCHECKs that this happened.
class KeyAttributeData {
 public:
  KeyAttributeData() noexcept = default;
  ~KeyAttributeData() {
    DCHECK(chunk_ == nullptr)
        << "KeyAttributeData destroyed without prior DestroyContents()";
  }

  KeyAttributeData(const KeyAttributeData&) = delete;
  KeyAttributeData& operator=(const KeyAttributeData&) = delete;

  KeyAttributeData(KeyAttributeData&& other) noexcept : chunk_(other.chunk_) {
    other.chunk_ = nullptr;
  }
  KeyAttributeData& operator=(KeyAttributeData&& other) noexcept {
    DCHECK(chunk_ == nullptr)
        << "Move-assigning into a non-empty KeyAttributeData would leak";
    chunk_ = other.chunk_;
    other.chunk_ = nullptr;
    return *this;
  }

  bool IsEmpty() const noexcept { return chunk_ == nullptr; }

 private:
  template <typename>
  friend class AttributeSlot;
  friend class KeyAttributeDataBuilder;
  friend class SchemaAttributeRegistry;

  void* chunk_ = nullptr;
};
static_assert(sizeof(KeyAttributeData) == sizeof(void*),
              "KeyAttributeData must be exactly one pointer wide");

// Per-IndexSchema registry of attribute slots. Indexes call Register<T>() at
// IndexSchema construction time and receive a fully-constructed
// AttributeSlot<T>. The slot id itself is internal.
class SchemaAttributeRegistry {
 public:
  SchemaAttributeRegistry() = default;
  SchemaAttributeRegistry(const SchemaAttributeRegistry&) = delete;
  SchemaAttributeRegistry& operator=(const SchemaAttributeRegistry&) = delete;

  // Register a payload type and return a fully-constructed slot handle.
  // Must be called before Finalize().
  template <typename T>
  AttributeSlot<T> Register();

  // Closes the registry. After this call no new slots may be registered.
  void Finalize() {
    CHECK(!finalized_) << "SchemaAttributeRegistry::Finalize called twice";
    finalized_ = true;
  }
  bool IsFinalized() const noexcept { return finalized_; }

  // Selectively destruct the present payloads of `data` and free its chunk.
  // After this call data.empty() == true. Safe to call on an already-empty
  // KeyAttributeData (no-op).
  void DestroyContents(KeyAttributeData& data) const;

 private:
  template <typename>
  friend class AttributeSlot;
  friend class KeyAttributeDataBuilder;

  struct SlotEntry {
    size_t size;
    size_t align;
    void (*destroy)(void*);
    void (*move_construct)(void* dst, void* src);
  };

  uint32_t SlotCount() const noexcept {
    return static_cast<uint32_t>(entries_.size());
  }
  size_t BitmapBytes() const noexcept { return (entries_.size() + 7) / 8; }
  size_t MaxAlign() const noexcept { return max_align_; }
  const SlotEntry& Get(uint32_t slot) const { return entries_[slot]; }

  // Returns the byte offset of slot's payload within the chunk, or
  // std::nullopt if the slot is not present. `chunk` must be non-null.
  std::optional<size_t> SlotOffset(const void* chunk, uint32_t slot) const;

  std::vector<SlotEntry> entries_;
  size_t max_align_ = 1;
  bool finalized_ = false;
};

// Per-key builder. Used during ingestion to accumulate per-attribute payloads
// in any order. MaterializeInto() allocates a single chunk, move-constructs
// each staged payload into final position, and writes the chunk pointer into
// the destination KeyAttributeData.
class KeyAttributeDataBuilder {
 public:
  explicit KeyAttributeDataBuilder(const SchemaAttributeRegistry& reg)
      : registry_(&reg) {}
  ~KeyAttributeDataBuilder();

  KeyAttributeDataBuilder(const KeyAttributeDataBuilder&) = delete;
  KeyAttributeDataBuilder& operator=(const KeyAttributeDataBuilder&) = delete;
  KeyAttributeDataBuilder(KeyAttributeDataBuilder&&) noexcept;

  // Allocates a single chunk and constructs all staged payloads at their final
  // positions. The destination must be empty (chunk_ == nullptr); call
  // SchemaAttributeRegistry::DestroyContents on it first if it had prior
  // contents. After this call the builder is consumed and contains nothing.
  void MaterializeInto(KeyAttributeData& out) &&;

 private:
  template <typename>
  friend class AttributeSlot;

  // Slot-id-keyed staging map. The buffer is a heap allocation aligned to the
  // payload type's alignment, holding one constructed instance.
  struct StagedSlot {
    void* buffer;
  };

  template <typename T, typename... Args>
  T& EmplaceSlot(uint32_t slot, Args&&... args);
  template <typename T>
  std::optional<std::reference_wrapper<T>> AccessSlot(uint32_t slot);
  bool HasSlot(uint32_t slot) const {
    return staged_.find(slot) != staged_.end();
  }

  const SchemaAttributeRegistry* registry_;
  absl::flat_hash_map<uint32_t, StagedSlot> staged_;
};

// Per-Index handle. Templated on the payload type the Index registered.
// Constructible only by SchemaAttributeRegistry::Register<T>().
template <typename T>
class AttributeSlot {
 public:
  AttributeSlot(const AttributeSlot&) = default;
  AttributeSlot& operator=(const AttributeSlot&) = default;

  std::optional<std::reference_wrapper<T>> Access(KeyAttributeData& d) const {
    if (d.IsEmpty()) {
      return std::nullopt;
    }
    auto offset = registry_->SlotOffset(d.chunk_, slot_);
    if (!offset) {
      return std::nullopt;
    }
    return std::ref(
        *reinterpret_cast<T*>(static_cast<char*>(d.chunk_) + *offset));
  }
  std::optional<std::reference_wrapper<const T>> Access(
      const KeyAttributeData& d) const {
    if (d.IsEmpty()) {
      return std::nullopt;
    }
    auto offset = registry_->SlotOffset(d.chunk_, slot_);
    if (!offset) {
      return std::nullopt;
    }
    return std::cref(*reinterpret_cast<const T*>(
        static_cast<const char*>(d.chunk_) + *offset));
  }

  template <typename... Args>
  T& EmplaceInBuilder(KeyAttributeDataBuilder& b, Args&&... args) const {
    return b.template EmplaceSlot<T>(slot_, std::forward<Args>(args)...);
  }
  std::optional<std::reference_wrapper<T>> AccessInBuilder(
      KeyAttributeDataBuilder& b) const {
    return b.template AccessSlot<T>(slot_);
  }

 private:
  friend class SchemaAttributeRegistry;
  AttributeSlot(const SchemaAttributeRegistry& reg, uint32_t slot) noexcept
      : registry_(&reg), slot_(slot) {}

  const SchemaAttributeRegistry* registry_;
  uint32_t slot_;
};

// === Templated method bodies ===

template <typename T>
AttributeSlot<T> SchemaAttributeRegistry::Register() {
  CHECK(!finalized_)
      << "SchemaAttributeRegistry::Register called after Finalize";
  static_assert(!std::is_reference_v<T>,
                "AttributeSlot payload type must not be a reference");
  const uint32_t slot = static_cast<uint32_t>(entries_.size());
  entries_.push_back(SlotEntry{
      sizeof(T), alignof(T),
      +[](void* p) { static_cast<T*>(p)->~T(); },
      +[](void* dst, void* src) {
        ::new (dst) T(std::move(*static_cast<T*>(src)));
      },
  });
  if (alignof(T) > max_align_) {
    max_align_ = alignof(T);
  }
  return AttributeSlot<T>(*this, slot);
}

template <typename T, typename... Args>
T& KeyAttributeDataBuilder::EmplaceSlot(uint32_t slot, Args&&... args) {
  const auto& entry = registry_->Get(slot);
  auto it = staged_.find(slot);
  if (it != staged_.end()) {
    entry.destroy(it->second.buffer);
    ::operator delete(it->second.buffer, std::align_val_t{entry.align});
    staged_.erase(it);
  }
  void* buf = ::operator new(entry.size, std::align_val_t{entry.align});
  T* obj = ::new (buf) T(std::forward<Args>(args)...);
  staged_.emplace(slot, StagedSlot{buf});
  return *obj;
}

template <typename T>
std::optional<std::reference_wrapper<T>> KeyAttributeDataBuilder::AccessSlot(
    uint32_t slot) {
  auto it = staged_.find(slot);
  if (it == staged_.end()) {
    return std::nullopt;
  }
  return std::ref(*static_cast<T*>(it->second.buffer));
}

}  // namespace valkey_search

#endif  // VALKEYSEARCH_SRC_SCHEMA_ATTRIBUTE_STORAGE_H_
