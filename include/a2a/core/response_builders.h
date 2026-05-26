// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/v1/a2a.pb.h"
#include "google/protobuf/struct.pb.h"

namespace a2a::core {

struct ArtifactOptions final {
  std::string artifact_id{};
  std::string name{};
  std::string description{};
};

struct FilePartOptions final {
  std::string filename{};
  std::string media_type{};
};

class ResponseBuilders final {
 public:
  static lf::a2a::v1::Artifact TextArtifact(std::string_view text, const ArtifactOptions& options = {});

  static lf::a2a::v1::Artifact RawFileArtifact(std::string_view raw_content, const FilePartOptions& file_options = {},
                                               const ArtifactOptions& options = {});

  static lf::a2a::v1::Artifact FileUrlArtifact(std::string_view url, const FilePartOptions& file_options = {},
                                               const ArtifactOptions& options = {});

  static lf::a2a::v1::Artifact StructuredDataArtifact(const google::protobuf::Value& data,
                                                      const ArtifactOptions& options = {},
                                                      const FilePartOptions& part_options = {});

  static void AddArtifactsWithPrimary(lf::a2a::v1::Task* task, lf::a2a::v1::Artifact primary_artifact,
                                      std::initializer_list<lf::a2a::v1::Artifact> additional_artifacts);
};

}  // namespace a2a::core
