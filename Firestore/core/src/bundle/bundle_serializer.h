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

#ifndef FIRESTORE_CORE_SRC_BUNDLE_BUNDLE_SERIALIZER_H_
#define FIRESTORE_CORE_SRC_BUNDLE_BUNDLE_SERIALIZER_H_

#include <string>
#include <utility>

#include "Firestore/core/src/bundle/bundle_document.h"
#include "Firestore/core/src/bundle/bundle_metadata.h"
#include "Firestore/core/src/bundle/bundled_document_metadata.h"
#include "Firestore/core/src/bundle/named_query.h"
#include "Firestore/core/src/core/core_fwd.h"
#include "Firestore/core/src/model/resource_path.h"
#include "Firestore/core/src/model/snapshot_version.h"
#include "Firestore/core/src/remote/serializer.h"
#include "Firestore/core/src/util/read_context.h"
#include "nlohmann/json.hpp"

namespace firebase {
namespace firestore {
namespace bundle {

class JsonReader : public util::ReadContext {
 public:
  const std::string& RequireString(const char* name,
                                   const nlohmann::json& json_object);
  const std::vector<nlohmann::json>& RequireArray(
      const char* name, const nlohmann::json& json_object);
  const nlohmann::json& Require(const char* child_name,
                                const nlohmann::json& json_object);

  double RequireDouble(const char* name, const nlohmann::json& value);
  template <typename int_type>
  int_type RequireInt(const char* name, const nlohmann::json& value);

  static bool OptionalBool(const char* name, const nlohmann::json& json_object);
};

/** A JSON serializer to deserialize Firestore Bundles. */
class BundleSerializer {
 public:
  explicit BundleSerializer(remote::Serializer serializer)
      : rpc_serializer_(std::move(serializer)) {
  }
  BundleMetadata DecodeBundleMetadata(JsonReader& context,
                                      const std::string& metadata) const;

  NamedQuery DecodeNamedQuery(JsonReader& context,
                              const std::string& named_query) const;

  BundledDocumentMetadata DecodeDocumentMetadata(
      JsonReader& context, const std::string& document_metadata) const;

  BundleDocument DecodeDocument(JsonReader& context,
                                const std::string& document) const;

 private:
  BundledQuery DecodeBundledQuery(JsonReader& context,
                                  const nlohmann::json& query) const;
  core::FilterList DecodeWhere(JsonReader& context,
                               const nlohmann::json& query) const;
  core::Filter DecodeFieldFilter(JsonReader& context,
                                 const nlohmann::json& filter) const;
  core::FilterList DecodeCompositeFilter(JsonReader& context,
                                         const nlohmann::json& filter) const;
  model::FieldValue DecodeValue(JsonReader& context,
                                const nlohmann::json& value) const;
  core::Bound DecodeBound(JsonReader& context,
                          const nlohmann::json& query,
                          const char* bound_name) const;
  model::ResourcePath DecodeName(JsonReader& context,
                                 const nlohmann::json& name) const;

  remote::Serializer rpc_serializer_;
  model::FieldValue DecodeReferenceValue(JsonReader& context,
                                         const std::string& value) const;
  model::FieldValue DecodeArrayValue(JsonReader& context,
                                     const nlohmann::json& array_json) const;
  model::FieldValue DecodeMapValue(JsonReader& context,
                                   const nlohmann::json& map_json) const;
};

}  // namespace bundle
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_BUNDLE_BUNDLE_SERIALIZER_H_
