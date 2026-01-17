/*
 * Copyright (c) 2026, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/kastore.h"

namespace valkey_search {
namespace kastore {

void SchemaBuilder::ReserveStorage(const IndexBase *index, size_t bytes) {
  CHECK(schema_.attribute_to_slot_.find(index) ==
        schema_.attribute_to_slot_.end());
  auto i = schema_.slot_.size();
  Schema::AttributeInfo info{.size_ = bytes, .index_ = index};
  schema_.attribute_to_slot_[index] = i;
  schema_.slot_.emplace_back(std::move(info));
}

Schema SchemaBuilder::Build() { return std::move(schema_); }

StorageBuilder::StorageBuilder(const Schema &schema) : schema_(schema) {}
//
// Initialize from an existing Storage
//
StorageBuilder::StorageBuilder(const Schema &schema, const Storage &storage)
    : schema_(schema) {
  storage.ForEachAttribute(
      [&](size_t slot, std::string_view data) { slots_[slot] = data; });
}

void StorageBuilder::UpsertAttribute(const IndexBase *index,
                                     std::string_view data) {
  slots_[schema_.Info(index).slot_] = data;
}

void StorageBuilder::DeleteAttribute(const IndexBase *index) {
  slots_.erase(schema_.Info(index).slot_);
}

void *StorageBuilder::GetAttribute(const IndexBase *index, size_t size) const {
  auto &info = schema_.Info(index);
  auto itr = slots_.find(info.slot_);
  return itr == slots_.end() ? nullptr : (void *)itr->second.data();
}

//
// The format of a built storage item varies.
//
// There are four possible formats, which are encoded as two bits
//
// In the dense format, the header is followed by an array of offsets (count
// elements in size). The array of offsets is followed by the actual data values
// packed as bytes. The offsets in the offset array are byte values relative to
// the start of the data section (i.e. the first data value will be zero). A not
// present attribute will have an offset of all 1's.
//
// In the sparse format, the header is followed by an array of (slot, offset)
// pairs (count elements in size). The slots are pre-sorted into ascending order
// so that a binary lookup can be done. The offset is a byte offset relative to
// the start of the data section. Missing values are simply not in the table.
//
// Current implementation limits total data size to 2^16 bytes. and 2^14
// attributes
//

template <typename T>
struct DenseFormat : Header {
  static constexpr T kMissingMarker = static_cast<T>(-1);
  T &Offset(size_t slot) {
    T *offset_start = reinterpret_cast<T *>(offset_start);
    CHECK(slot < count);
    return offset_start[slot];
  }
  const T &Offset(size_t slot) const {
    T *offset_start = reinterpret_cast<T *>(offset_start);
    CHECK(slot < count);
    return offset_start[slot];
  }
  char *DataStart() { return reinterpret_cast<char *>(Offset(count)); }
  char *DataStart() const { return reinterpret_cast<char *>(Offset(count)); }
  char *GetSlot(size_t slot) {
    CHECK(slot < count);
    auto offset = Offset(slot);
    return (offset == kMissingMarker) ? nullptr : DataStart() + offset;
  }
  static Header *Build(const std::map<size_t, std::string> &slots,
                       size_t data_size) {
    auto num_slots = slots.empty() ? 0 : slots.rbegin()->first + 1;
    CHECK(data_size < std::numeric_limits<T>::max());
    size_t total = sizeof(DenseFormat) + (num_slots * sizeof(T) + data_size);
    DenseFormat *storage = reinterpret_cast<DenseFormat *>(new char[total]);
    storage->offset_size = sizeof(T) == 1 ? 0 : 1;
    storage->sparse = 0;
    storage->count = num_slots;
    size_t data_offset = 0;
    for (auto i = 0; i < num_slots; ++i) {
      auto itr = slots.find(i);
      if (itr == slots.end()) {
        storage->Offset(i) = kMissingMarker;
      } else {
        storage->Offset(i) = data_offset;
        memcpy(storage->DataStart() + data_offset, itr->second.data(),
               itr->second.size());
        data_offset += itr->second.size();
      }
    }
    CHECK(data_offset == data_size);
    return storage;
  }
  char *AccessAttribute(size_t slot, size_t len) const {
    auto offset = Offset(slot);
    if (offset == kMissingMarker) {
      return nullptr;
    }
    return DataStart() + offset;
  }
};

// TBD on SparseFormat

Storage StorageBuilder::Build() {
  auto data_size = 0;
  for (const auto &[slot, data] : slots_) {
    data_size += data.size();
  }
  if (data_size < 0xFF) {
    return {DenseFormat<uint8_t>::Build(slots_, data_size)};
  } else if (data_size < 0xFFFF) {
    return {DenseFormat<uint16_t>::Build(slots_, data_size)};
  } else {
    CHECK(false) << "Data size " << data_size << " Too large";
  }
}

char *Storage::AccessAttribute(const Schema &schema, const IndexBase *index,
                               size_t len) const {
  auto &info = schema.Info(index);
  CHECK(info.size_ == len);
  if (header_->sparse) {
    CHECK(false);
  } else {
    if (header_->offset_size) {
      return reinterpret_cast<const DenseFormat<uint16_t> *>(this)
          ->AccessAttribute(info.slot_, len);
    } else {
      return reinterpret_cast<const DenseFormat<uint8_t> *>(this)
          ->AccessAttribute(info.slot_, len);
    }
  }
}

};  // namespace kastore
};  // namespace valkey_search