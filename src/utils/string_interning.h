/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEYSEARCH_SRC_UTILS_STRING_INTERNING_H_
#define VALKEYSEARCH_SRC_UTILS_STRING_INTERNING_H_

#include <absl/container/btree_map.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/node_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "gtest/gtest_prod.h"
#include "src/utils/allocator.h"
#include "vmsdk/src/memory_tracker.h"

namespace valkey_search {

class InternedStringImpl;
class InternedStringPtr;

//
// An interned string. This is a reference-counted object of variable size.
// Either the string data is stored inline OR for externally allocated strings
// a pointer to the allocation is stored.
//
class InternedString {
 public:
  friend class StringInternStore;
  absl::string_view Str() const;
  operator absl::string_view() const { return Str(); }
  absl::string_view operator*() const { return Str(); }

  // Private except for specialized out-of-line construction.
 protected:
  InternedString() = default;
  InternedString(const InternedString &) = delete;
  InternedString &operator=(const InternedString &) = delete;
  InternedString(InternedString &&) = delete;
  InternedString &operator=(InternedString &&) = delete;
  ~InternedString() = default;

  friend class InternedStringPtr;
  static InternedString *Constructor(absl::string_view str,
                                     Allocator *allocator);
  void Destructor();
  void IncrementRefCount() {
    ref_count_.fetch_add(1, std::memory_order_seq_cst);
  }
  void DecrementRefCount();

  uint32_t RefCount() const {
    return ref_count_.load(std::memory_order_seq_cst);
  }

  //
  // For stats.
  //
  bool IsInline() const { return is_inline_; }
  size_t Allocated() const {
    size_t bytes = 0;
#ifndef SAN_BUILD
    // Crashes with sanitizer builds.
    bytes += ValkeyModule_MallocUsableSize(const_cast<InternedString *>(this));
#endif
    if (!is_inline_) {
      // Can't actually do a UsableSize of allocated blobs, so use the size.
      bytes += length_;
    }
    return bytes;
  }
  //
  // Data layout. there's an 8 byte header followed by either the inline
  // string data or a pointer to externally allocated string data.
  //
  // The declaration below only provides a placeholder for the variable-length
  // data_ member. In the allocated case there's[0] actually a pointer stored
  // here. Future optimizations could further shrink the header size by using a
  // variable size for the length_ field.
  //
  std::atomic<uint32_t> ref_count_;
  uint32_t length_ : 31;
  uint32_t is_inline_ : 1;
  char data_[0];
};

static_assert(sizeof(InternedString) == 8,
              "InternedString has unexpected padding");

//
// A smart pointer for InternedString that manages reference counting.
//
class InternedStringPtr {
 public:
  InternedStringPtr() = default;
  InternedStringPtr(const InternedStringPtr &other) : impl_(other.impl_) {
    if (impl_) {
      impl_->IncrementRefCount();
    }
  }
  InternedStringPtr(InternedStringPtr &&other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
  }
  InternedStringPtr &operator=(const InternedStringPtr &other) {
    if (this != &other) {
      if (impl_) {
        impl_->DecrementRefCount();
      }
      impl_ = other.impl_;
      if (impl_) {
        impl_->IncrementRefCount();
      }
    }
    return *this;
  }
  InternedStringPtr &operator=(InternedStringPtr &&other) noexcept {
    if (this != &other) {
      if (impl_) {
        impl_->DecrementRefCount();
      }
      impl_ = other.impl_;
      other.impl_ = nullptr;
    }
    return *this;
  }

  InternedStringPtr &operator=(void *other) noexcept {
    CHECK(!other);  // Only nullptr is allowed
    if (impl_) {
      impl_->DecrementRefCount();
    }
    impl_ = nullptr;
    return *this;
  }

  auto operator<=>(const InternedStringPtr &other) const = default;

  size_t Hash() const { return absl::HashOf(impl_); }

  InternedString &operator*() { return *impl_; }
  InternedString *operator->() { return impl_; }
  operator bool() const { return impl_ != nullptr; }
  const InternedString &operator*() const { return *impl_; }
  const InternedString *operator->() const { return impl_; }
  ~InternedStringPtr() {
    if (impl_) {
      impl_->DecrementRefCount();
    }
    impl_ = nullptr;
  }

  size_t RefCount() const { return impl_ ? impl_->RefCount() : 0; }

 private:
  InternedStringPtr(InternedString *impl) : impl_(impl) {}
  InternedString *impl_{nullptr};
  friend class StringInternStore;
};

inline std::ostream &operator<<(std::ostream &os,
                                const InternedStringPtr &str) {
  return str ? os << str->Str() : os << "<null>";
}

}  // namespace valkey_search

// std::hash specialization for InternedStringPtr. Defined here, before any
// flat_hash_set<InternedStringPtr> instantiation, so the BagOfInternedStringPtrs
// definition below (which references InternedStringSet) can compile.
template <>
struct std::hash<valkey_search::InternedStringPtr> {
  std::size_t operator()(const valkey_search::InternedStringPtr &sp) const {
    return sp.Hash();
  }
};

namespace valkey_search {

template <typename T>
using InternedStringHashMap = absl::flat_hash_map<InternedStringPtr, T>;
using InternedStringSet = absl::flat_hash_set<InternedStringPtr>;

template <typename T>
using InternedStringNodeHashMap = absl::node_hash_map<InternedStringPtr, T>;

//
// BagOfInternedStringPtrs is a space-optimized drop-in replacement for
// InternedStringSet (i.e. absl::flat_hash_set<InternedStringPtr>) tuned for the
// extremely common case of zero or one elements. The whole object is exactly
// 8 bytes:
//
//   storage_ == 0                   -> empty
//   storage_ != 0, bit 0 clear      -> single-element mode; the slot IS a
//                                      clean InternedStringPtr (no tag bits
//                                      anywhere -- usable in place via
//                                      reinterpret_cast)
//   storage_ has bit 0 set          -> multi-element mode; (storage_ & ~1)
//                                      is the InternedStringSet*
//
// Tag location: bit 0 is free because absl::flat_hash_set allocated via `new`
// has alignment 8. The tag must be stripped before dereferencing the multi
// pointer (a tagged value is misaligned and points one byte past the real
// allocation). Single mode, by contrast, stores a clean InternedStringPtr
// directly, so the bag can hand out a stable reference to it without any
// masking or copying.
//
// AddressSanitizer note: the tagged value (multi mode) sets only the low bit,
// so it still points one byte INTO the heap allocation it represents. ASAN's
// leak scanner uses interior-pointer detection and recognizes such pointers
// as references to the surrounding allocation, so the owning pointer is not
// hidden from the leak checker.
//
// This class works exclusively through the public InternedStringPtr API. It
// is not a friend of InternedStringPtr or its underlying string type. All
// ref-count traffic flows through InternedStringPtr's public copy/move
// constructors and destructor, invoked via placement new and explicit
// destruction on the storage slot.
//
// The bag is intentionally non-copyable and non-equality-comparable; only
// move construction and move assignment are provided.
//
class BagOfInternedStringPtrs {
 public:
  using value_type = InternedStringPtr;
  using key_type = InternedStringPtr;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  class const_iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = InternedStringPtr;
    using reference = const InternedStringPtr &;
    using pointer = const InternedStringPtr *;
    using difference_type = std::ptrdiff_t;

    const_iterator() = default;

    reference operator*() const {
      if (tag_ == Tag::kSingle) {
        // bag_->storage_ in single mode IS a clean InternedStringPtr;
        // SingleRef() exposes it as a stable reference into the bag's slot.
        return bag_->SingleRef();
      }
      return *multi_;
    }
    pointer operator->() const {
      if (tag_ == Tag::kSingle) {
        return &bag_->SingleRef();
      }
      return multi_.operator->();
    }
    const_iterator &operator++() {
      if (tag_ == Tag::kSingle) {
        tag_ = Tag::kEnd;
      } else if (tag_ == Tag::kMulti) {
        ++multi_;
      }
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator tmp(*this);
      ++(*this);
      return tmp;
    }
    bool operator==(const const_iterator &other) const {
      if (tag_ != other.tag_) {
        return false;
      }
      switch (tag_) {
        case Tag::kEnd:
          return true;
        case Tag::kSingle:
          return bag_ == other.bag_;
        case Tag::kMulti:
          return multi_ == other.multi_;
      }
      return false;
    }
    bool operator!=(const const_iterator &other) const {
      return !(*this == other);
    }

   private:
    friend class BagOfInternedStringPtrs;
    enum class Tag { kEnd, kSingle, kMulti };

    const_iterator(Tag tag, const BagOfInternedStringPtrs *bag,
                   InternedStringSet::const_iterator multi = {})
        : tag_(tag), bag_(bag), multi_(multi) {}

    Tag tag_ = Tag::kEnd;
    const BagOfInternedStringPtrs *bag_ = nullptr;
    InternedStringSet::const_iterator multi_;
  };
  using iterator = const_iterator;
  using pointer = const InternedStringPtr *;
  using const_pointer = const InternedStringPtr *;
  using reference = const InternedStringPtr &;
  using const_reference = const InternedStringPtr &;

  BagOfInternedStringPtrs() = default;
  BagOfInternedStringPtrs(const BagOfInternedStringPtrs &) = delete;
  BagOfInternedStringPtrs &operator=(const BagOfInternedStringPtrs &) = delete;
  BagOfInternedStringPtrs(BagOfInternedStringPtrs &&other) noexcept
      : storage_(other.storage_) {
    other.storage_ = 0;
  }
  BagOfInternedStringPtrs &operator=(BagOfInternedStringPtrs &&other) noexcept {
    if (this != &other) {
      clear();
      storage_ = other.storage_;
      other.storage_ = 0;
    }
    return *this;
  }
  ~BagOfInternedStringPtrs() { clear(); }

  size_type size() const {
    if (IsEmpty()) {
      return 0;
    }
    if (IsSingle()) {
      return 1;
    }
    return AsMulti()->size();
  }
  bool empty() const { return storage_ == 0; }

  void clear() {
    if (IsSingle()) {
      // storage_ in single mode IS a clean InternedStringPtr; in-place
      // destruct it to drop the ref count, then zero out the slot.
      reinterpret_cast<InternedStringPtr *>(&storage_)->~InternedStringPtr();
    } else if (IsMulti()) {
      delete AsMulti();
    }
    storage_ = 0;
  }

  bool contains(const InternedStringPtr &key) const {
    if (IsEmpty()) {
      return false;
    }
    if (IsSingle()) {
      // SingleRef() exposes storage_ as an InternedStringPtr; default
      // operator== is a bit-exact compare of the underlying impl_ values.
      return SingleRef() == key;
    }
    return AsMulti()->contains(key);
  }

  const_iterator find(const InternedStringPtr &key) const {
    if (IsEmpty()) {
      return end();
    }
    if (IsSingle()) {
      if (SingleRef() == key) {
        return const_iterator(const_iterator::Tag::kSingle, this);
      }
      return end();
    }
    auto it = AsMulti()->find(key);
    if (it == AsMulti()->end()) {
      return end();
    }
    return const_iterator(const_iterator::Tag::kMulti, this, it);
  }

  std::pair<const_iterator, bool> insert(const InternedStringPtr &key) {
    if (IsEmpty()) {
      // Public copy ctor bumps the ref count and writes a clean
      // InternedStringPtr directly into storage_. No tagging needed.
      new (&storage_) InternedStringPtr(key);
      return {const_iterator(const_iterator::Tag::kSingle, this), true};
    }
    if (IsSingle()) {
      if (SingleRef() == key) {
        return {const_iterator(const_iterator::Tag::kSingle, this), false};
      }
      // Promote: move the lone clean InternedStringPtr into a new set, then
      // insert the new key.
      auto *set = new InternedStringSet();
      set->insert(
          std::move(*reinterpret_cast<InternedStringPtr *>(&storage_)));
      // The move ctor nulled impl_, so storage_ is now 0.
      auto [it, inserted] = set->insert(key);
      SetMulti(set);
      return {const_iterator(const_iterator::Tag::kMulti, this, it), inserted};
    }
    auto [it, inserted] = AsMulti()->insert(key);
    return {const_iterator(const_iterator::Tag::kMulti, this, it), inserted};
  }
  std::pair<const_iterator, bool> insert(InternedStringPtr &&key) {
    if (IsEmpty()) {
      // Public move ctor adopts the ref count without bump/drop.
      new (&storage_) InternedStringPtr(std::move(key));
      return {const_iterator(const_iterator::Tag::kSingle, this), true};
    }
    if (IsSingle()) {
      if (SingleRef() == key) {
        return {const_iterator(const_iterator::Tag::kSingle, this), false};
      }
      auto *set = new InternedStringSet();
      set->insert(
          std::move(*reinterpret_cast<InternedStringPtr *>(&storage_)));
      auto [it, inserted] = set->insert(std::move(key));
      SetMulti(set);
      return {const_iterator(const_iterator::Tag::kMulti, this, it), inserted};
    }
    auto [it, inserted] = AsMulti()->insert(std::move(key));
    return {const_iterator(const_iterator::Tag::kMulti, this, it), inserted};
  }

  size_type erase(const InternedStringPtr &key) {
    if (IsEmpty()) {
      return 0;
    }
    if (IsSingle()) {
      if (SingleRef() == key) {
        reinterpret_cast<InternedStringPtr *>(&storage_)->~InternedStringPtr();
        storage_ = 0;
        return 1;
      }
      return 0;
    }
    size_type n = AsMulti()->erase(key);
    if (n > 0) {
      DemoteIfPossible();
    }
    return n;
  }

  // erase(iterator) follows flat_hash_set semantics: any outstanding iterator
  // (including pos) is invalidated. absl::flat_hash_set::erase(it) returns
  // void (it does not provide a "next" iterator like std::unordered_set), so
  // we mirror that here and always return end(). Callers that need to
  // continue iterating after an erase must restart from begin().
  const_iterator erase(const_iterator pos) {
    if (pos.tag_ == const_iterator::Tag::kSingle) {
      reinterpret_cast<InternedStringPtr *>(&storage_)->~InternedStringPtr();
      storage_ = 0;
      return end();
    }
    AsMulti()->erase(pos.multi_);
    DemoteIfPossible();
    return end();
  }

  const_iterator begin() const {
    if (IsEmpty()) {
      return end();
    }
    if (IsSingle()) {
      return const_iterator(const_iterator::Tag::kSingle, this);
    }
    return const_iterator(const_iterator::Tag::kMulti, this,
                          AsMulti()->begin());
  }
  const_iterator end() const {
    if (IsMulti()) {
      return const_iterator(const_iterator::Tag::kMulti, this,
                            AsMulti()->end());
    }
    return const_iterator();
  }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  void swap(BagOfInternedStringPtrs &other) noexcept {
    std::swap(storage_, other.storage_);
  }

 private:
  // Bit 0 marks the multi-element mode. The single mode stores a clean
  // InternedStringPtr directly with no tag bits.
  static constexpr uintptr_t kMultiTag = uintptr_t(1);

  uintptr_t storage_ = 0;

  bool IsEmpty() const { return storage_ == 0; }
  bool IsMulti() const { return (storage_ & kMultiTag) != 0; }
  bool IsSingle() const {
    return storage_ != 0 && (storage_ & kMultiTag) == 0;
  }

  // Stable reference to the bag's single-mode slot, viewed as an
  // InternedStringPtr. Precondition: IsSingle().
  const InternedStringPtr &SingleRef() const {
    return *reinterpret_cast<const InternedStringPtr *>(&storage_);
  }
  InternedStringSet *AsMulti() const {
    return reinterpret_cast<InternedStringSet *>(storage_ & ~kMultiTag);
  }
  void SetMulti(InternedStringSet *p) {
    storage_ = reinterpret_cast<uintptr_t>(p) | kMultiTag;
  }

  void DemoteIfPossible() {
    if (!IsMulti()) {
      return;
    }
    auto *set = AsMulti();
    if (set->size() == 0) {
      delete set;
      storage_ = 0;
    } else if (set->size() == 1) {
      // Move-construct the lone InternedStringPtr into our slot. The public
      // move ctor adopts the ref count without bump/drop, leaving storage_ as
      // a clean InternedStringPtr (single mode is identified by an untagged
      // non-zero storage_).
      InternedStringPtr lone = std::move(*set->begin());
      delete set;
      new (&storage_) InternedStringPtr(std::move(lone));
    }
  }
};
static_assert(sizeof(BagOfInternedStringPtrs) == 8,
              "BagOfInternedStringPtrs must fit in 8 bytes");

class StringInternStore {
 public:
  friend class InternedString;
  static StringInternStore &Instance();
  //
  // Interns the given string. If an identical string has already been
  // interned, returns a pointer to the existing interned string. Otherwise,
  // creates a new interned string.
  //
  static InternedStringPtr Intern(absl::string_view str,
                                  Allocator *allocator = nullptr);

  static int64_t GetMemoryUsage();

  size_t UniqueStrings() const {
    absl::MutexLock lock(&mutex_);
    return str_to_interned_.size();
  }

  struct Stats {
    struct BucketStats {
      size_t count_{0};      // Number of entries in this bucket
      size_t bytes_{0};      // Total bytes of string data (no overhead)
      size_t allocated_{0};  // Total bytes of allocated data
    };
    absl::btree_map<int, BucketStats> by_ref_stats_;
    absl::btree_map<int, BucketStats> by_size_stats_;
    BucketStats inline_total_stats_;
    BucketStats out_of_line_total_stats_;
  };
  Stats GetStats() const;

 private:
  static MemoryPool memory_pool_;

  StringInternStore() = default;
  bool Release(InternedString *str);
  InternedStringPtr InternImpl(absl::string_view str,
                               Allocator *allocator = nullptr);
  struct InternedStringPtrFullHash {
    std::size_t operator()(const InternedStringPtr &sp) const {
      return absl::HashOf(sp->Str());
    }
  };

  struct InternedStringPtrFullEqual {
    bool operator()(const InternedStringPtr &lhs,
                    const InternedStringPtr &rhs) const {
      return lhs->Str() == rhs->Str();
    }
  };
  absl::flat_hash_set<InternedStringPtr, InternedStringPtrFullHash,
                      InternedStringPtrFullEqual>
      str_to_interned_ ABSL_GUARDED_BY(mutex_);
  mutable absl::Mutex mutex_;

  // Used for testing.
  static void SetMemoryUsage(int64_t value) {
    memory_pool_.Reset();
    memory_pool_.Add(value);
  }

  static StringInternStore *MakeInstance();

  FRIEND_TEST(ValkeySearchTest, Info);
};

}  // namespace valkey_search

#endif  // VALKEYSEARCH_SRC_UTILS_STRING_INTERNING_H_
