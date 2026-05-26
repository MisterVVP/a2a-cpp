// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/response_builders.h"

namespace a2a::core {
namespace {

void ApplyArtifactOptions(lf::a2a::v1::Artifact* artifact, const ArtifactOptions& options) {
  artifact->set_artifact_id(options.artifact_id);
  if (!options.name.empty()) {
    artifact->set_name(options.name);
  }
  if (!options.description.empty()) {
    artifact->set_description(options.description);
  }
}

void ApplyFileOptions(lf::a2a::v1::Part* part, const FilePartOptions& options) {
  if (!options.filename.empty()) {
    part->set_filename(options.filename);
  }
  if (!options.media_type.empty()) {
    part->set_media_type(options.media_type);
  }
}

}  // namespace

lf::a2a::v1::Artifact ResponseBuilders::TextArtifact(std::string_view text, const ArtifactOptions& options) {
  lf::a2a::v1::Artifact artifact;
  ApplyArtifactOptions(&artifact, options);
  artifact.add_parts()->set_text(std::string{text});
  return artifact;
}

lf::a2a::v1::Artifact ResponseBuilders::RawFileArtifact(std::string_view raw_content,
                                                        const FilePartOptions& file_options,
                                                        const ArtifactOptions& options) {
  lf::a2a::v1::Artifact artifact;
  ApplyArtifactOptions(&artifact, options);
  auto* part = artifact.add_parts();
  part->set_raw(std::string{raw_content});
  ApplyFileOptions(part, file_options);
  return artifact;
}

lf::a2a::v1::Artifact ResponseBuilders::FileUrlArtifact(std::string_view url, const FilePartOptions& file_options,
                                                        const ArtifactOptions& options) {
  lf::a2a::v1::Artifact artifact;
  ApplyArtifactOptions(&artifact, options);
  auto* part = artifact.add_parts();
  part->set_url(std::string{url});
  ApplyFileOptions(part, file_options);
  return artifact;
}

lf::a2a::v1::Artifact ResponseBuilders::StructuredDataArtifact(const google::protobuf::Value& data,
                                                               const ArtifactOptions& options,
                                                               const FilePartOptions& part_options) {
  lf::a2a::v1::Artifact artifact;
  ApplyArtifactOptions(&artifact, options);
  auto* part = artifact.add_parts();
  *part->mutable_data() = data;
  ApplyFileOptions(part, part_options);
  return artifact;
}

void ResponseBuilders::AddArtifactsWithPrimary(lf::a2a::v1::Task* task, lf::a2a::v1::Artifact primary_artifact,
                                               std::initializer_list<lf::a2a::v1::Artifact> additional_artifacts) {
  if (task == nullptr) {
    return;
  }

  task->clear_artifacts();
  *task->add_artifacts() = std::move(primary_artifact);
  for (const auto& artifact : additional_artifacts) {
    *task->add_artifacts() = artifact;
  }
}

}  // namespace a2a::core
