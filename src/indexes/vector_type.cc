/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/indexes/vector_type.h"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "src/index_schema.pb.h"
#include "src/indexes/vector_base.h"
#include "third_party/hnswlib/hnswlib.h"
#include "third_party/hnswlib/space_ip.h"
#include "third_party/hnswlib/space_l2.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/type_conversions.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"

namespace valkey_search::indexes {

namespace {

// Constructs the right hnswlib space for T + metric. Kept private to
// this TU -- callers reach it via VectorType<T>::Init below.
template <typename T>
std::unique_ptr<hnswlib::SpaceInterface<T>> CreateSpace(
    int dimensions, data_model::DistanceMetric distance_metric) {
  if constexpr (std::is_same_v<T, float>) {
    if (distance_metric == data_model::DistanceMetric::DISTANCE_METRIC_COSINE ||
        distance_metric == data_model::DistanceMetric::DISTANCE_METRIC_IP) {
      return std::make_unique<hnswlib::InnerProductSpace>(dimensions);
    }
    return std::make_unique<hnswlib::L2Space>(dimensions);
  }
  DCHECK(false) << "no matching space for T";
  return std::make_unique<hnswlib::L2Space>(dimensions);
}

// Compile-time mapping from T to its data_model::VectorDataType enum.
// If a new element type is added, add its arm here and both leaves'
// ToProtoImpl / RespondWithInfoImpl bodies stay unchanged.
template <typename T>
constexpr data_model::VectorDataType VectorDataTypeEnumFor() {
  if constexpr (std::is_same_v<T, float>) {
    return data_model::VECTOR_DATA_TYPE_FLOAT32;
  } else {
    // Force a compile error rather than a silent runtime UNSPECIFIED --
    // adding a new T without adding an arm here is a bug we want caught
    // at build time.
    static_assert(sizeof(T) == 0, "no VectorDataType enum for this T");
  }
}

}  // namespace

template <typename T>
void VectorType<T>::Init(data_model::DistanceMetric distance_metric) {
  space_ = CreateSpace<T>(dimensions_, distance_metric);
  distance_metric_ = distance_metric;
  if (distance_metric == data_model::DistanceMetric::DISTANCE_METRIC_COSINE) {
    normalize_ = true;
  }
}

template <typename T>
void VectorType<T>::EmitDataTypeInfo(ValkeyModuleCtx *ctx) const {
  ValkeyModule_ReplyWithSimpleString(ctx, "data_type");
  ValkeyModule_ReplyWithSimpleString(
      ctx, LookupKeyByValue(*kVectorDataTypeByStr, VectorDataTypeEnumFor<T>())
               .data());
}

template <typename T>
void VectorType<T>::SetProtoDataType(
    data_model::VectorIndex *vector_index_proto) const {
  vector_index_proto->set_vector_data_type(VectorDataTypeEnumFor<T>());
}

template <typename T>
vmsdk::UniqueValkeyString VectorType<T>::NormalizeStringRecord(
    vmsdk::UniqueValkeyString record) const {
  auto record_str = vmsdk::ToStringView(record.get());
  if (absl::ConsumePrefix(&record_str, "[")) {
    absl::ConsumeSuffix(&record_str, "]");
  }
  std::vector<std::string> float_strings =
      absl::StrSplit(record_str, ',', absl::SkipWhitespace());
  std::string binary_string;
  binary_string.reserve(float_strings.size() * sizeof(T));
  for (const auto &float_str : float_strings) {
    float value;
    if (!absl::SimpleAtof(float_str, &value)) {
      return nullptr;
    }
    if constexpr (std::is_same_v<T, float>) {
      binary_string += std::string((char *)&value, sizeof(T));
    } else {
      // For future element types, the caller-visible ASCII "1.0" query
      // string must be re-encoded to the on-disk width. Adding the
      // conversion here (float -> T) is a one-line change per new type.
      static_assert(sizeof(T) == 0,
                    "NormalizeStringRecord not yet wired for this T");
    }
  }
  return vmsdk::MakeUniqueValkeyString(binary_string);
}

template class VectorType<float>;

}  // namespace valkey_search::indexes
