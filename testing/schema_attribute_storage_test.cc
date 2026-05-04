/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/schema_attribute_storage.h"

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include "gtest/gtest.h"

namespace valkey_search {
namespace {

// Type-trait checks (compile-time).
static_assert(sizeof(KeyAttributeData) == sizeof(void*),
              "KeyAttributeData must be exactly one pointer wide");
static_assert(!std::is_copy_constructible_v<KeyAttributeData>);
static_assert(!std::is_copy_assignable_v<KeyAttributeData>);
static_assert(std::is_nothrow_move_constructible_v<KeyAttributeData>);
static_assert(std::is_nothrow_move_assignable_v<KeyAttributeData>);

// A type with a non-trivial destructor that records destruct counts and
// pins itself to a fixed address (asserts on move).
struct Tracked {
  static int alive_count;
  static int destruct_count;
  Tracked* self_at_construct;
  int value;

  explicit Tracked(int v) : self_at_construct(this), value(v) {
    ++alive_count;
  }
  Tracked(Tracked&& other) noexcept
      : self_at_construct(this), value(other.value) {
    ++alive_count;
  }
  Tracked& operator=(Tracked&&) = delete;
  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;
  ~Tracked() {
    --alive_count;
    ++destruct_count;
    // Once stored in the chunk, the object must remain at the same address
    // for its entire lifetime.
    EXPECT_EQ(self_at_construct, this);
  }
};
int Tracked::alive_count = 0;
int Tracked::destruct_count = 0;

class SchemaAttributeStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    Tracked::alive_count = 0;
    Tracked::destruct_count = 0;
  }
};

TEST_F(SchemaAttributeStorageTest, EmptyHolderIsZeroSizedSemantically) {
  KeyAttributeData data;
  EXPECT_TRUE(data.IsEmpty());
}

TEST_F(SchemaAttributeStorageTest, RegisterReturnsHandlesSlotsAreOpaque) {
  SchemaAttributeRegistry reg;
  AttributeSlot<int> a = reg.Register<int>();
  AttributeSlot<double> b = reg.Register<double>();
  AttributeSlot<std::string> c = reg.Register<std::string>();
  reg.Finalize();
  // Just verifying handles are usable through the API; slot ids are not
  // exposed and not asserted on.
  KeyAttributeDataBuilder builder(reg);
  a.EmplaceInBuilder(builder, 42);
  b.EmplaceInBuilder(builder, 3.14);
  c.EmplaceInBuilder(builder, "hello");

  KeyAttributeData data;
  std::move(builder).MaterializeInto(data);

  EXPECT_EQ(a.Access(data)->get(), 42);
  EXPECT_DOUBLE_EQ(b.Access(data)->get(), 3.14);
  EXPECT_EQ(c.Access(data)->get(), "hello");

  reg.DestroyContents(data);
  EXPECT_TRUE(data.IsEmpty());
}

TEST_F(SchemaAttributeStorageTest, BuilderOrderIndependent) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<int>();
  auto b = reg.Register<int>();
  auto c = reg.Register<int>();
  reg.Finalize();

  KeyAttributeDataBuilder builder(reg);
  // Emplace in reverse order from registration.
  c.EmplaceInBuilder(builder, 30);
  a.EmplaceInBuilder(builder, 10);
  b.EmplaceInBuilder(builder, 20);

  KeyAttributeData data;
  std::move(builder).MaterializeInto(data);
  EXPECT_EQ(a.Access(data)->get(), 10);
  EXPECT_EQ(b.Access(data)->get(), 20);
  EXPECT_EQ(c.Access(data)->get(), 30);
  reg.DestroyContents(data);
}

TEST_F(SchemaAttributeStorageTest, AbsentSlotReturnsNullopt) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<int>();
  auto b = reg.Register<int>();
  auto c = reg.Register<int>();
  reg.Finalize();

  KeyAttributeDataBuilder builder(reg);
  // Only populate b.
  b.EmplaceInBuilder(builder, 99);
  KeyAttributeData data;
  std::move(builder).MaterializeInto(data);

  EXPECT_FALSE(a.Access(data).has_value());
  EXPECT_TRUE(b.Access(data).has_value());
  EXPECT_EQ(b.Access(data)->get(), 99);
  EXPECT_FALSE(c.Access(data).has_value());

  reg.DestroyContents(data);
}

TEST_F(SchemaAttributeStorageTest, SparseAcrossManyBytesOfBitmap) {
  SchemaAttributeRegistry reg;
  // Register 20 slots so the bitmap is 3 bytes.
  std::vector<AttributeSlot<int>> slots;
  slots.reserve(20);
  for (int i = 0; i < 20; ++i) {
    slots.push_back(reg.Register<int>());
  }
  reg.Finalize();

  KeyAttributeDataBuilder builder(reg);
  // Populate slot 0 and slot 17 only — bitmap byte 1 is entirely zero,
  // exercising the sparse-skip in the bitmap walk.
  slots[0].EmplaceInBuilder(builder, 1000);
  slots[17].EmplaceInBuilder(builder, 1700);

  KeyAttributeData data;
  std::move(builder).MaterializeInto(data);

  EXPECT_EQ(slots[0].Access(data)->get(), 1000);
  EXPECT_FALSE(slots[1].Access(data).has_value());
  EXPECT_FALSE(slots[8].Access(data).has_value());
  EXPECT_FALSE(slots[16].Access(data).has_value());
  EXPECT_EQ(slots[17].Access(data)->get(), 1700);
  EXPECT_FALSE(slots[18].Access(data).has_value());

  reg.DestroyContents(data);
}

TEST_F(SchemaAttributeStorageTest, DestructorRunsOncePerStoredInstance) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<Tracked>();
  auto b = reg.Register<Tracked>();
  reg.Finalize();

  KeyAttributeData data;
  {
    KeyAttributeDataBuilder builder(reg);
    a.EmplaceInBuilder(builder, 1);
    b.EmplaceInBuilder(builder, 2);
    // staging buffers are alive: 2.
    EXPECT_EQ(Tracked::alive_count, 2);
    std::move(builder).MaterializeInto(data);
    // Materialize move-constructs into the chunk and destroys the staging
    // copies, so alive count should remain 2 (one per slot).
    EXPECT_EQ(Tracked::alive_count, 2);
  }

  EXPECT_EQ(a.Access(data)->get().value, 1);
  EXPECT_EQ(b.Access(data)->get().value, 2);

  reg.DestroyContents(data);
  EXPECT_EQ(Tracked::alive_count, 0);
}

TEST_F(SchemaAttributeStorageTest, DestroyContentsIsIdempotent) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<int>();
  reg.Finalize();

  KeyAttributeData data;
  KeyAttributeDataBuilder builder(reg);
  a.EmplaceInBuilder(builder, 7);
  std::move(builder).MaterializeInto(data);

  reg.DestroyContents(data);
  EXPECT_TRUE(data.IsEmpty());
  // Second call must be a safe no-op.
  reg.DestroyContents(data);
  EXPECT_TRUE(data.IsEmpty());
}

TEST_F(SchemaAttributeStorageTest, BuilderDtorCleansUpStagedNonMaterialized) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<Tracked>();
  reg.Finalize();
  {
    KeyAttributeDataBuilder builder(reg);
    a.EmplaceInBuilder(builder, 5);
    EXPECT_EQ(Tracked::alive_count, 1);
    // Builder goes out of scope without materializing.
  }
  EXPECT_EQ(Tracked::alive_count, 0);
}

TEST_F(SchemaAttributeStorageTest, EmptyBuilderProducesEmptyHolder) {
  SchemaAttributeRegistry reg;
  reg.Register<int>();
  reg.Register<double>();
  reg.Finalize();

  KeyAttributeData data;
  KeyAttributeDataBuilder builder(reg);
  std::move(builder).MaterializeInto(data);
  EXPECT_TRUE(data.IsEmpty());
  // Safe to destroy an empty holder via the registry too.
  reg.DestroyContents(data);
}

TEST_F(SchemaAttributeStorageTest, ZeroSlotRegistryProducesEmptyHolder) {
  SchemaAttributeRegistry reg;
  reg.Finalize();
  KeyAttributeData data;
  KeyAttributeDataBuilder builder(reg);
  std::move(builder).MaterializeInto(data);
  EXPECT_TRUE(data.IsEmpty());
}

TEST_F(SchemaAttributeStorageTest, MovingHolderDoesNotMovePayload) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<Tracked>();
  reg.Finalize();

  KeyAttributeData src;
  KeyAttributeDataBuilder builder(reg);
  a.EmplaceInBuilder(builder, 99);
  std::move(builder).MaterializeInto(src);
  Tracked* before = &a.Access(src)->get();

  KeyAttributeData dst(std::move(src));
  EXPECT_TRUE(src.IsEmpty());
  Tracked* after = &a.Access(dst)->get();
  // The payload object did not move when the holder moved.
  EXPECT_EQ(before, after);

  reg.DestroyContents(dst);
}

TEST_F(SchemaAttributeStorageTest, MixedAlignmentPayloadsRoundTrip) {
  // Verify that payloads of different alignments are placed correctly.
  SchemaAttributeRegistry reg;
  auto a = reg.Register<uint8_t>();
  auto b = reg.Register<uint64_t>();   // alignof = 8
  auto c = reg.Register<uint16_t>();   // alignof = 2
  reg.Finalize();

  KeyAttributeData data;
  KeyAttributeDataBuilder builder(reg);
  a.EmplaceInBuilder(builder, uint8_t{0xAB});
  b.EmplaceInBuilder(builder, uint64_t{0xDEADBEEFCAFEBABEULL});
  c.EmplaceInBuilder(builder, uint16_t{0x1234});
  std::move(builder).MaterializeInto(data);

  EXPECT_EQ(a.Access(data)->get(), uint8_t{0xAB});
  EXPECT_EQ(b.Access(data)->get(), uint64_t{0xDEADBEEFCAFEBABEULL});
  EXPECT_EQ(c.Access(data)->get(), uint16_t{0x1234});

  // Verify alignment of each payload at its address.
  auto* pb = &b.Access(data)->get();
  EXPECT_EQ(reinterpret_cast<uintptr_t>(pb) % alignof(uint64_t), 0u);
  auto* pc = &c.Access(data)->get();
  EXPECT_EQ(reinterpret_cast<uintptr_t>(pc) % alignof(uint16_t), 0u);

  reg.DestroyContents(data);
}

TEST_F(SchemaAttributeStorageTest, GetKeyCountStartsAtZero) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<int>();
  auto b = reg.Register<int>();
  reg.Finalize();
  EXPECT_EQ(a.GetKeyCount(), 0u);
  EXPECT_EQ(b.GetKeyCount(), 0u);
}

TEST_F(SchemaAttributeStorageTest, GetKeyCountTracksMaterializeAndDestroy) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<int>();
  auto b = reg.Register<int>();
  auto c = reg.Register<int>();
  reg.Finalize();

  // Materialize three keys with various subsets of slots populated.
  KeyAttributeData k1, k2, k3;
  {
    KeyAttributeDataBuilder builder(reg);
    a.EmplaceInBuilder(builder, 1);
    b.EmplaceInBuilder(builder, 2);
    std::move(builder).MaterializeInto(k1);  // a, b
  }
  {
    KeyAttributeDataBuilder builder(reg);
    b.EmplaceInBuilder(builder, 20);
    c.EmplaceInBuilder(builder, 30);
    std::move(builder).MaterializeInto(k2);  // b, c
  }
  {
    KeyAttributeDataBuilder builder(reg);
    a.EmplaceInBuilder(builder, 100);
    std::move(builder).MaterializeInto(k3);  // a
  }

  EXPECT_EQ(a.GetKeyCount(), 2u);  // k1, k3
  EXPECT_EQ(b.GetKeyCount(), 2u);  // k1, k2
  EXPECT_EQ(c.GetKeyCount(), 1u);  // k2

  reg.DestroyContents(k1);
  EXPECT_EQ(a.GetKeyCount(), 1u);
  EXPECT_EQ(b.GetKeyCount(), 1u);
  EXPECT_EQ(c.GetKeyCount(), 1u);

  reg.DestroyContents(k2);
  EXPECT_EQ(a.GetKeyCount(), 1u);
  EXPECT_EQ(b.GetKeyCount(), 0u);
  EXPECT_EQ(c.GetKeyCount(), 0u);

  reg.DestroyContents(k3);
  EXPECT_EQ(a.GetKeyCount(), 0u);
  EXPECT_EQ(b.GetKeyCount(), 0u);
  EXPECT_EQ(c.GetKeyCount(), 0u);
}

TEST_F(SchemaAttributeStorageTest, GetKeyCountUnchangedByEmptyBuilder) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<int>();
  reg.Finalize();
  KeyAttributeData data;
  KeyAttributeDataBuilder builder(reg);
  std::move(builder).MaterializeInto(data);
  EXPECT_TRUE(data.IsEmpty());
  EXPECT_EQ(a.GetKeyCount(), 0u);
}

TEST_F(SchemaAttributeStorageTest, GetKeyCountUnchangedByMovingHolder) {
  SchemaAttributeRegistry reg;
  auto a = reg.Register<int>();
  reg.Finalize();
  KeyAttributeData src;
  KeyAttributeDataBuilder builder(reg);
  a.EmplaceInBuilder(builder, 7);
  std::move(builder).MaterializeInto(src);
  EXPECT_EQ(a.GetKeyCount(), 1u);
  KeyAttributeData dst(std::move(src));
  // Moving the holder does not allocate or release a chunk; the counter is
  // unchanged.
  EXPECT_EQ(a.GetKeyCount(), 1u);
  reg.DestroyContents(dst);
  EXPECT_EQ(a.GetKeyCount(), 0u);
}

#ifndef NDEBUG
TEST_F(SchemaAttributeStorageTest, RegisterAfterFinalizeFatal) {
  SchemaAttributeRegistry reg;
  reg.Finalize();
  EXPECT_DEATH({ reg.Register<int>(); }, "Register called after Finalize");
}
#endif

}  // namespace
}  // namespace valkey_search
