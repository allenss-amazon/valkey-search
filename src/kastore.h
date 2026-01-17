/*
 * Copyright (c) 2026, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEYSEARCH_SRC_KASTORE_H_
#define VALKEYSEARCH_SRC_KASTORE_H_

/*

KAStore is a repository for per-key per-attribute information.

Conceptually, for each key in an index_schema, there is a place for each
attribute to store information that's unique to that key/attribute instance.
KAStore provides a space efficient implementation of this concept.

KAStore specifically also optimizes for the case when a key/attribute is
missing and therefore needs to have no storage. This provides a space efficient
implementation for index schemas that have a large number of sparsely populated
attributes. In particular, this allows the implementation of a negation query
operation without burdening each attribute with a private map of missing
keys. That map is effectively free with KAStore.

When an index_schema is created, a KAStore::Schema is created which provides a
per-IndexSchema registry of the required space and constructor/destructor
requirements for that space. After index creation, this schema is immutable.

The IndexSchema also contains a map<Key, IndexKeyInfo>. The IndexKeyInfo
contains per-key information for the entire schema (like the mutation sequence
number) By adding a KAStore::Storage instance to that object, it's now extended
to provide per-key-per-attribute information storage.

*/

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

#include "src/indexes/index_base.h"

namespace valkey_search {
namespace kastore {

using valkey_search::indexes::IndexBase;

struct Schema {
  struct AttributeInfo {
    size_t slot_;  // slot number
    size_t size_;  // Number of bytes reserved for this instance
    const IndexBase *index_;
  };
  std::vector<AttributeInfo> slot_;
  absl::flat_hash_map<const IndexBase *, size_t> attribute_to_slot_;
  const AttributeInfo &Info(const IndexBase *index) const {
    auto itr = attribute_to_slot_.find(index);
    CHECK(itr != attribute_to_slot_.end()) << "Attribute not found";
    return slot_[itr->second];
  }
};

class SchemaBuilder {
 public:
  // Only allow POD types, non-POD types can be supported later.
  template <typename T>
  void ReserveStorage(const IndexBase *index) {
    static_assert(std::is_pod<T>::value, "Only POD data is allowed");
    ReserveStorage(index, sizeof(T));
  }
  Schema Build();

 private:
  Schema schema_;
  void ReserveStorage(const IndexBase *index, size_t size);
};

/*

The structure of a storage record isn't mutable. In other words, you can't add
or remove attributes. You can modify the contents of an existing attribute
inplace as much as you want. But if you want to add or remove an attribute you
create a new object -- possibly seeded from an existing one.
*/

class Storage;

class StorageBuilder {
 public:
  // Create from empty
  StorageBuilder(const Schema &schema);
  // Create from existing Storage
  StorageBuilder(const Schema &schema, const Storage &storage);
  // Create or overwrite existing Attribute
  // This will assert if the size of the data doesn't match what was reserved
  template <typename T>
  void UpsertAttribute(const IndexBase *index, const T &data) {
    static_assert(std::is_pod<T>::value, "Only POD data is allowed");
    UpsertAttribute(
        index,
        std::string_view(reinterpret_cast<const char *>(&data), sizeof(T)));
  }
  // Delete attribute. If the attribute doesn't exist, do nothing.
  void DeleteAttribute(const IndexBase *index);
  // Access existing data if it's there
  template <typename T>
  std::optional<T *> GetAttribute(IndexBase *index) const {
    static_assert(std::is_pod<T>::value, " Only POD data is allowed");
    auto result = GetAttribute(index, sizeof(T));
    if (result) {
      return reinterpret_cast<T *>(result);
    } else {
      return std::nullopt;
    }
  }
  // Make a new Storage
  Storage Build();

 private:
  void *GetAttribute(const IndexBase *index, size_t size) const;
  void UpsertAttribute(const IndexBase *index, std::string_view data);
  std::map<size_t, std::string> slots_;
  const Schema &schema_;
};

struct Header {
  uint16_t
      offset_size : 1;   // 0 => offsets are 8 bits, 1 => offsets are 16 bits
  uint16_t sparse : 1;   // 0 => dense, 1 => sparse
  uint16_t count : 14;   // Number of elements that are present
  char offset_start[0];  // Start of offset table
};

class Storage {
 public:
  // Access the data for this attribute.
  // It's legitimate to modify the bytes in the Object
  template <typename T>
  std::optional<T *> AccessAttribute(const Schema &schema,
                                     const IndexBase *index) const {
    static_assert(std::is_pod<T>::value, "Only POD data is allowed");
    auto result = AccessAttribute(schema, index, sizeof(T));
    if (result) {
      return reinterpret_cast<T *>(result);
    } else {
      return std::nullopt;
    }
  }

  //
  // Iterate over the object, the callback is invoked for every Attribute that's
  // present
  //
  void ForEachAttribute(const Schema &schema,
                        std::function<void(const IndexBase *)> cb) const;

 private:
  char *AccessAttribute(const Schema &schema, const IndexBase *index,
                        size_t size) const;
  friend class StorageBuilder;
  // Internal iterate where we just get slots and size
  void ForEachAttribute(const Schema &schema,
                        std::function<void(size_t, std::string_view)> cb) const;
  std::unique_ptr<const Header> header_;
  Storage(const Header *h);
};

};  // namespace kastore

};  // namespace valkey_search

#endif