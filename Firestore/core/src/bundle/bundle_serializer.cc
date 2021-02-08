/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/src/bundle/bundle_serializer.h"

#include <memory>
#include <vector>

#include "Firestore/core/src/core/bound.h"
#include "Firestore/core/src/core/direction.h"
#include "Firestore/core/src/core/field_filter.h"
#include "Firestore/core/src/core/filter.h"
#include "Firestore/core/src/core/order_by.h"
#include "Firestore/core/src/core/query.h"
#include "Firestore/core/src/core/target.h"
#include "Firestore/core/src/model/document.h"
#include "Firestore/core/src/model/field_path.h"
#include "Firestore/core/src/model/field_value.h"
#include "Firestore/core/src/model/resource_path.h"
#include "Firestore/core/src/nanopb/byte_string.h"
#include "Firestore/core/src/timestamp_internal.h"
#include "Firestore/core/src/util/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/numbers.h"
#include "absl/time/time.h"

namespace firebase {
namespace firestore {
namespace bundle {
namespace {

using absl::Base64Unescape;
using absl::FromUnixSeconds;
using absl::Nanoseconds;
using absl::ParseTime;
using absl::SimpleAtod;
using absl::SimpleAtoi;
using absl::Time;
using absl::ToUnixSeconds;
using core::Bound;
using core::Direction;
using core::FieldFilter;
using core::Filter;
using core::FilterList;
using core::LimitType;
using core::OrderBy;
using core::OrderByList;
using core::Query;
using core::Target;
using immutable::AppendOnlyList;
using model::Document;
using model::DocumentKey;
using model::FieldPath;
using model::FieldValue;
using model::ObjectValue;
using model::ResourcePath;
using model::SnapshotVersion;
using nanopb::ByteString;
using nanopb::Reader;
using nlohmann::json;
using util::ReadContext;
using util::StatusOr;

const std::string& empty_string() {
  static auto* empty = new std::string;
  return *empty;
}

template <typename T>
const std::vector<T>& empty_vector() {
  static auto* empty = new std::vector<T>;
  return *empty;
}

json Parse(const std::string& s) {
  return json::parse(s, /*callback=*/nullptr, /*allow_exception=*/false);
}

Timestamp DecodeTimestamp(JsonReader& reader, const json& version) {
  StatusOr<Timestamp> decoded;
  if (version.is_string()) {
    Time time;
    std::string err;
    bool ok = ParseTime(absl::RFC3339_full,
                        version.get_ref<const std::string&>(), &time, &err);
    if (ok) {
      decoded = TimestampInternal::FromUntrustedTime(time);
    } else {
      reader.Fail("Parsing timestamp failed with error: " + err);
      return {};
    }
  } else {
    decoded = TimestampInternal::FromUntrustedSecondsAndNanos(
        reader.RequireInt<int64_t>("seconds", version),
        reader.RequireInt<int32_t>("nanos", version));
  }

  if (!decoded.ok()) {
    reader.set_status(decoded.status());
    return {};
  }
  return decoded.ConsumeValueOrDie();
}

SnapshotVersion DecodeSnapshotVersion(JsonReader& reader, const json& version) {
  return SnapshotVersion(DecodeTimestamp(reader, version));
}

void VerifyStructuredQuery(JsonReader& reader, const json& query) {
  if (!query.is_object()) {
    reader.Fail("'structuredQuery' is not an object as expected.");
    return;
  }
  if (query.contains("select")) {
    reader.Fail(
        "Queries with 'select' statements are not supported in bundles");
    return;
  }
  if (!query.contains("from")) {
    reader.Fail("Query does not have a 'from' collection");
    return;
  }
  if (query.contains("offset")) {
    reader.Fail("Queries with 'offset' are not supported in bundles");
    return;
  }
}

/**
 * Decodes a json object into the given `path` and `group` reference.
 */
void DecodeCollectionSource(JsonReader& reader,
                            const json& from_json,
                            ResourcePath& parent,
                            std::string& group) {
  const auto& from = from_json.get_ref<const std::vector<json>&>();
  if (from.size() != 1) {
    reader.Fail(
        "Only queries with a single 'from' clause are supported by the SDK");
    return;
  }
  const auto& collection_selector = from.at(0);
  const auto& collection_id =
      reader.RequireString("collectionId", collection_selector);
  bool all_descendants =
      reader.OptionalBool("allDescendants", collection_selector);

  if (all_descendants) {
    group = collection_id;
  } else {
    parent = parent.Append(collection_id);
  }
}

FieldPath DecodeFieldReference(JsonReader& reader, const json& field) {
  if (!field.is_object()) {
    reader.Fail("'field' should be an json object, but it is not");
    return FieldPath();
  }

  const auto& field_path = reader.RequireString("fieldPath", field);
  auto result = FieldPath::FromServerFormat(field_path);

  if (!result.ok()) {
    reader.set_status(result.status());
    return FieldPath();
  } else {
    return result.ConsumeValueOrDie();
  }
}

Filter::Operator DecodeFieldFilterOperator(JsonReader& reader,
                                           const std::string& op) {
  if (op == "LESS_THAN") {
    return Filter::Operator::LessThan;
  } else if (op == "LESS_THAN_OR_EQUAL") {
    return Filter::Operator::LessThanOrEqual;
  } else if (op == "EQUAL") {
    return Filter::Operator::Equal;
  } else if (op == "NOT_EQUAL") {
    return Filter::Operator::NotEqual;
  } else if (op == "GREATER_THAN") {
    return Filter::Operator::GreaterThan;
  } else if (op == "GREATER_THAN_OR_EQUAL") {
    return Filter::Operator::GreaterThanOrEqual;
  } else if (op == "ARRAY_CONTAINS") {
    return Filter::Operator::ArrayContains;
  } else if (op == "IN") {
    return Filter::Operator::In;
  } else if (op == "ARRAY_CONTAINS_ANY") {
    return Filter::Operator::ArrayContainsAny;
  } else if (op == "NOT_IN") {
    return Filter::Operator::NotIn;
  } else {
    reader.Fail("Operator in filter is not valid: " + op);
    // We have to return something.
    return Filter::Operator::Equal;
  }
}

Filter InvalidFilter() {
  // The exact value doesn't matter. Note that there's no way to create the base
  // class `Filter`, so it has to be one of the derived classes.
  return FieldFilter::Create({}, {}, {});
}

Filter DecodeUnaryFilter(JsonReader& reader, const json& filter) {
  FieldPath path =
      DecodeFieldReference(reader, reader.Require("field", filter));
  std::string op = reader.RequireString("op", filter);

  // Return early if !ok(), because `FieldFilter::Create` will abort with
  // invalid inputs.
  if (!reader.ok()) {
    return InvalidFilter();
  }

  if (op == "IS_NAN") {
    return FieldFilter::Create(std::move(path), Filter::Operator::Equal,
                               FieldValue::Nan());
  } else if (op == "IS_NULL") {
    return FieldFilter::Create(std::move(path), Filter::Operator::Equal,
                               FieldValue::Null());
  } else if (op == "IS_NOT_NAN") {
    return FieldFilter::Create(std::move(path), Filter::Operator::NotEqual,
                               FieldValue::Nan());
  } else if (op == "IS_NOT_NULL") {
    return FieldFilter::Create(std::move(path), Filter::Operator::NotEqual,
                               FieldValue::Null());
  }

  reader.Fail("Unexpected unary filter operator: " + op);
  return InvalidFilter();
}

OrderByList DecodeOrderBy(JsonReader& reader, const json& query) {
  OrderByList result;
  for (const auto& order_by : reader.RequireArray("orderBy", query)) {
    FieldPath path =
        DecodeFieldReference(reader, reader.Require("field", order_by));

    std::string direction_string = "ASCENDING";
    if (order_by.contains("direction")) {
      direction_string = reader.RequireString("direction", order_by);
    }
    if (direction_string != "DESCENDING" && direction_string != "ASCENDING") {
      reader.Fail("'direction' value is invalid: " + direction_string);
      return OrderByList();
    }

    Direction direction = direction_string == "ASCENDING"
                              ? Direction::Ascending
                              : Direction::Descending;

    result = result.push_back(OrderBy(std::move(path), direction));
  }

  return result;
}

int32_t DecodeLimit(JsonReader& reader, const json& query) {
  int32_t limit = Target::kNoLimit;
  if (query.contains("limit")) {
    if (!query.at("limit").is_number_integer()) {
      reader.Fail("'limit' is not encoded as a valid integer");
      return limit;
    }
    limit = query.at("limit").get<int32_t>();
  }

  return limit;
}

LimitType DecodeLimitType(JsonReader& reader, const json& query) {
  std::string limit_type = "FIRST";
  if (query.contains("limitType")) {
    limit_type = reader.RequireString("limitType", query);
  }

  if (limit_type == "FIRST") {
    return LimitType::First;
  } else if (limit_type == "LAST") {
    return LimitType::Last;
  } else {
    reader.Fail("'limitType' is not encoded as a recognizable value");
    return LimitType::None;
  }
}

FieldValue DecodeGeoPointValue(JsonReader& reader, const json& geo_json) {
  double latitude = 0;
  if (geo_json.contains("latitude")) {
    latitude = reader.RequireDouble("latitude", geo_json);
  }

  double longitude = 0;
  if (geo_json.contains("longitude")) {
    longitude = reader.RequireDouble("longitude", geo_json);
  }
  return FieldValue::FromGeoPoint(GeoPoint(latitude, longitude));
}

FieldValue DecodeBytesValue(JsonReader& reader,
                            const std::string& bytes_string) {
  std::string decoded;
  if (!Base64Unescape(bytes_string, &decoded)) {
    reader.Fail("Failed to decode bytesValue string into binary form");
    return FieldValue();
  }
  return FieldValue::FromBlob(ByteString((decoded)));
}

}  // namespace

// Mark: JsonReader

const std::string& JsonReader::RequireString(const char* name,
                                             const json& json_object) {
  if (json_object.contains(name)) {
    const json& child = json_object.at(name);
    if (child.is_string()) {
      return child.get_ref<const std::string&>();
    }
  }

  Fail("'%s' is missing or is not a string", name);
  return empty_string();
}

const std::vector<json>& JsonReader::RequireArray(const char* name,
                                                  const json& json_object) {
  if (json_object.contains(name)) {
    const json& child = json_object.at(name);
    if (child.is_array()) {
      return child.get_ref<const std::vector<json>&>();
    }
  }

  Fail("'%s' is missing or is not a string", name);
  return empty_vector<json>();
}

bool JsonReader::OptionalBool(const char* name, const json& json_object) {
  return json_object.contains(name) && json_object.at(name).is_boolean() &&
         json_object.at(name).get<bool>();
}

const nlohmann::json& JsonReader::Require(const char* child_name,
                                          const json& json_object) {
  if (!json_object.contains(child_name)) {
    Fail("Missing child '%s'", child_name);
    return json_object;
  }
  return json_object.at(child_name);
}

double JsonReader::RequireDouble(const char* name, const json& json_object) {
  if (json_object.contains(name)) {
    const json& value = json_object.at(name);
    if (value.is_number()) {
      return value.get<double>();
    }

    double result = 0;
    if (value.is_string()) {
      const auto& s = value.get_ref<const std::string&>();
      auto ok = SimpleAtod(s, &result);
      if (!ok) {
        Fail("Failed to parse into double: " + s);
      }

      return result;
    }
  }

  Fail("'%s' is missing or is not a double", name);
  return 0.0;
}

template <typename int_type>
int_type JsonReader::RequireInt(const char* name, const json& json_object) {
  if (json_object.contains(name)) {
    const json& value = json_object.at(name);
    if (value.is_number_integer()) {
      return value.get<int_type>();
    }

    int_type result = 0;
    if (value.is_string()) {
      const auto& s = value.get_ref<const std::string&>();
      auto ok = SimpleAtoi<int_type>(s, &result);
      if (!ok) {
        Fail("Failed to parse into integer: " + s);
      }

      return result;
    }
  }

  Fail("'%s' is missing or is not a double", name);
  return 0;
}

// Mark: BundleSerializer

BundleMetadata BundleSerializer::DecodeBundleMetadata(
    JsonReader& reader, const std::string& metadata_string) const {
  const json& metadata = Parse(metadata_string);

  if (metadata.is_discarded()) {
    reader.Fail("Failed to parse string into json: " + metadata_string);
    return BundleMetadata();
  }

  return BundleMetadata(
      reader.RequireString("id", metadata),
      reader.RequireInt<uint32_t>("version", metadata),
      DecodeSnapshotVersion(reader, reader.Require("createTime", metadata)),
      reader.RequireInt<uint32_t>("totalDocuments", metadata),
      reader.RequireInt<uint64_t>("totalBytes", metadata));
}

NamedQuery BundleSerializer::DecodeNamedQuery(
    JsonReader& reader, const std::string& named_query_string) const {
  const json& named_query = Parse(named_query_string);

  if (named_query.is_discarded()) {
    reader.Fail("Failed to parse string into json: " + named_query_string);
    return NamedQuery();
  }

  return NamedQuery(
      reader.RequireString("name", named_query),
      DecodeBundledQuery(reader, reader.Require("bundledQuery", named_query)),
      DecodeSnapshotVersion(reader, reader.Require("readTime", named_query)));
}

BundledQuery BundleSerializer::DecodeBundledQuery(
    JsonReader& reader, const nlohmann::json& query) const {
  const json& structured_query = reader.Require("structuredQuery", query);
  VerifyStructuredQuery(reader, structured_query);
  if (!reader.ok()) {
    return BundledQuery();
  }

  ResourcePath parent = DecodeName(reader, reader.Require("parent", query));
  std::string collection_group_string;
  DecodeCollectionSource(reader, structured_query.at("from"), parent,
                         collection_group_string);
  std::shared_ptr<std::string> collection_group;
  if (!collection_group_string.empty()) {
    collection_group = std::make_shared<std::string>(collection_group_string);
  }

  auto filters = DecodeWhere(reader, structured_query);
  auto order_bys = DecodeOrderBy(reader, structured_query);

  auto start_at_bound = DecodeBound(reader, structured_query, "startAt");
  std::shared_ptr<Bound> start_at;
  if (!start_at_bound.position().empty()) {
    start_at = std::make_shared<Bound>(std::move(start_at_bound));
  }

  auto end_at_bound = DecodeBound(reader, structured_query, "endAt");
  std::shared_ptr<Bound> end_at;
  if (!end_at_bound.position().empty()) {
    end_at = std::make_shared<Bound>(std::move(end_at_bound));
  }

  int32_t limit = DecodeLimit(reader, structured_query);
  LimitType limit_type = DecodeLimitType(reader, query);

  return BundledQuery(Target(std::move(parent), std::move(collection_group),
                             std::move(filters), std::move(order_bys), limit,
                             std::move(start_at), std::move(end_at)),
                      limit_type);
}

ResourcePath BundleSerializer::DecodeName(JsonReader& reader,
                                          const json& document_name) const {
  if (!document_name.is_string()) {
    reader.Fail("Document name is not a string.");
    return ResourcePath();
  }
  auto path =
      ResourcePath::FromString(document_name.get_ref<const std::string&>());
  if (!rpc_serializer_.IsLocalResourceName(path)) {
    reader.Fail("Resource name is not valid for current instance: " +
                path.CanonicalString());
    return ResourcePath();
  }
  return path.PopFirst(5);
}

FilterList BundleSerializer::DecodeWhere(JsonReader& reader,
                                         const json& query) const {
  // Absent 'where' is a valid case.
  if (!query.contains("where")) {
    return FilterList();
  }

  const auto& where = query.at("where");
  if (!where.is_object()) {
    reader.Fail("Query's 'where' clause is not a json object.");
    return FilterList();
  }

  FilterList result;
  if (where.contains("compositeFilter")) {
    return DecodeCompositeFilter(reader, where.at("compositeFilter"));
  } else if (where.contains("fieldFilter")) {
    return result.push_back(DecodeFieldFilter(reader, where.at("fieldFilter")));
  } else if (where.contains("unaryFilter")) {
    return result.push_back(DecodeUnaryFilter(reader, where.at("unaryFilter")));
  } else {
    reader.Fail("'where' does not have valid filter");
    return {};
  }
}

Filter BundleSerializer::DecodeFieldFilter(JsonReader& reader,
                                           const json& filter) const {
  FieldPath path =
      DecodeFieldReference(reader, reader.Require("field", filter));

  const auto& op_string = reader.RequireString("op", filter);
  auto op = DecodeFieldFilterOperator(reader, op_string);

  FieldValue value = DecodeValue(reader, reader.Require("value", filter));

  // Return early if !ok(), because `FieldFilter::Create` will abort with
  // invalid inputs.
  if (!reader.ok()) {
    return InvalidFilter();
  }

  return FieldFilter::Create(path, op, value);
}

FilterList BundleSerializer::DecodeCompositeFilter(JsonReader& reader,
                                                   const json& filter) const {
  if (reader.RequireString("op", filter) != "AND") {
    reader.Fail("The SDK only supports composite filters of type 'AND'");
    return {};
  }

  auto filters = reader.RequireArray("filters", filter);
  FilterList result;
  for (const auto& f : filters) {
    result = result.push_back(
        DecodeFieldFilter(reader, reader.Require("fieldFilter", f)));
    if (!reader.ok()) {
      return {};
    }
  }

  return result;
}

Bound BundleSerializer::DecodeBound(JsonReader& reader,
                                    const json& query,
                                    const char* bound_name) const {
  Bound default_bound = Bound({}, false);
  if (!query.contains(bound_name)) {
    return default_bound;
  }

  const json& bound_json = reader.Require(bound_name, query);
  bool before = reader.OptionalBool("before", bound_json);

  std::vector<FieldValue> positions;

  for (const auto& value : reader.RequireArray("values", bound_json)) {
    positions.push_back(DecodeValue(reader, value));
  }

  return Bound(std::move(positions), before);
}

FieldValue BundleSerializer::DecodeValue(JsonReader& reader,
                                         const json& value) const {
  if (!value.is_object()) {
    reader.Fail("'value' is not encoded as JSON object");
    return FieldValue();
  }

  if (value.contains("nullValue")) {
    return FieldValue::Null();
  } else if (value.contains("booleanValue")) {
    auto val = value.at("booleanValue");
    if (!val.is_boolean()) {
      reader.Fail("'booleanValue' is not encoded as a valid boolean");
      return FieldValue();
    }
    return FieldValue::FromBoolean(val.get<bool>());
  } else if (value.contains("integerValue")) {
    return FieldValue::FromInteger(
        reader.RequireInt<int64_t>("integerValue", value));
  } else if (value.contains("doubleValue")) {
    return FieldValue::FromDouble(reader.RequireDouble("doubleValue", value));
  } else if (value.contains("timestampValue")) {
    auto val = DecodeTimestamp(reader, value.at("timestampValue"));
    return FieldValue::FromTimestamp(val);
  } else if (value.contains("stringValue")) {
    auto val = reader.RequireString("stringValue", value);
    return FieldValue::FromString(std::move(val));
  } else if (value.contains("bytesValue")) {
    return DecodeBytesValue(reader, reader.RequireString("bytesValue", value));
  } else if (value.contains("referenceValue")) {
    return DecodeReferenceValue(reader,
                                reader.RequireString("referenceValue", value));
  } else if (value.contains("geoPointValue")) {
    return DecodeGeoPointValue(reader, value.at("geoPointValue"));
  } else if (value.contains("arrayValue")) {
    return DecodeArrayValue(reader, value.at("arrayValue"));
  } else if (value.contains("mapValue")) {
    return DecodeMapValue(reader, value.at("mapValue"));
  } else {
    reader.Fail("Failed to decode value, no type is recognized");
    return FieldValue();
  }
}

FieldValue BundleSerializer::DecodeMapValue(JsonReader& reader,
                                            const json& map_json) const {
  if (!map_json.is_object() || !map_json.contains("fields")) {
    reader.Fail("mapValue is not a valid map");
    return FieldValue();
  }
  const auto& fields = map_json.at("fields");
  if (!fields.is_object()) {
    reader.Fail("mapValue's 'field' is not a valid map");
    return FieldValue();
  }

  immutable::SortedMap<std::string, FieldValue> field_values;
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    field_values =
        field_values.insert(it.key(), DecodeValue(reader, it.value()));
  }

  return FieldValue::FromMap(std::move(field_values));
}

FieldValue BundleSerializer::DecodeArrayValue(JsonReader& reader,
                                              const json& array_json) const {
  const auto& values = reader.RequireArray("values", array_json);
  std::vector<FieldValue> field_values;
  for (const json& json_value : values) {
    field_values.push_back(DecodeValue(reader, json_value));
  }
  if (!reader.ok()) {
    return FieldValue();
  }

  return FieldValue::FromArray(std::move(field_values));
}

FieldValue BundleSerializer::DecodeReferenceValue(
    JsonReader& reader, const std::string& ref_string) const {
  // Check if ref_string is indeed a valid string passed in.
  if (!reader.ok()) {
    return FieldValue();
  }

  return rpc_serializer_.DecodeReference(&reader, ref_string);
}

BundledDocumentMetadata BundleSerializer::DecodeDocumentMetadata(
    JsonReader& reader, const std::string& document_metadata_string) const {
  const json& document_metadata = Parse(document_metadata_string);

  if (document_metadata.is_discarded()) {
    reader.Fail("Failed to parse string into json: " +
                document_metadata_string);
    return {};
  }

  ResourcePath path =
      DecodeName(reader, reader.Require("name", document_metadata));
  // Return early if !ok(), `DocumentKey` aborts with invalid inputs.
  if (!reader.ok()) {
    return {};
  }
  DocumentKey key = DocumentKey(path);

  SnapshotVersion read_time = DecodeSnapshotVersion(
      reader, reader.Require("readTime", document_metadata));

  bool exists = reader.OptionalBool("exists", document_metadata);

  std::vector<std::string> queries;
  for (const json& query : reader.RequireArray("queries", document_metadata)) {
    if (!query.is_string()) {
      reader.Fail("Query name should be encoded as string");
      return {};
    }

    queries.push_back(query.get<std::string>());
  }

  return BundledDocumentMetadata(std::move(key), read_time, exists,
                                 std::move(queries));
}

BundleDocument BundleSerializer::DecodeDocument(
    JsonReader& reader, const std::string& document_string) const {
  const json& document = Parse(document_string);

  if (document.is_discarded()) {
    reader.Fail("Failed to parse document string into json: " +
                document_string);
    return BundleDocument{};
  }

  ResourcePath path = DecodeName(reader, reader.Require("name", document));
  // Return early if !ok(), `DocumentKey` aborts with invalid inputs.
  if (!reader.ok()) {
    return BundleDocument{};
  }
  DocumentKey key = DocumentKey(path);

  SnapshotVersion update_time =
      DecodeSnapshotVersion(reader, reader.Require("updateTime", document));

  auto map_value = DecodeMapValue(reader, document);

  return BundleDocument(Document(ObjectValue::FromMap(map_value.object_value()),
                                 std::move(key), update_time,
                                 model::DocumentState::kSynced));
}

}  // namespace bundle
}  // namespace firestore
}  // namespace firebase
