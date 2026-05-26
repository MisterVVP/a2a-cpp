// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include "a2a/core/response_builders.h"
#include "google/protobuf/struct.pb.h"

namespace {

constexpr std::string_view kTextPayload = "hello";
constexpr std::string_view kFilePayload = "raw bytes";
constexpr std::string_view kFileUrl = "https://example.test/result.txt";
constexpr std::string_view kArtifactId = "artifact-1";
constexpr std::string_view kArtifactName = "primary";
constexpr std::string_view kArtifactDescription = "desc";
constexpr std::string_view kFilename = "result.txt";
constexpr std::string_view kMediaType = "text/plain";
constexpr std::string_view kDataField = "key";
constexpr std::string_view kDataValue = "value";

TEST(ResponseBuildersTest, TextArtifactBuildsSingleTextPart) {
  const auto artifact = a2a::core::ResponseBuilders::TextArtifact(
      kTextPayload, {.artifact_id = std::string{kArtifactId}, .name = std::string{kArtifactName}});

  EXPECT_EQ(artifact.artifact_id(), kArtifactId);
  EXPECT_EQ(artifact.name(), kArtifactName);
  ASSERT_EQ(artifact.parts_size(), 1);
  EXPECT_EQ(artifact.parts(0).text(), kTextPayload);
}

TEST(ResponseBuildersTest, RawFileArtifactBuildsRawPartAndFileMetadata) {
  const auto artifact = a2a::core::ResponseBuilders::RawFileArtifact(
      kFilePayload,
      {.filename = std::string{kFilename}, .media_type = std::string{kMediaType}},
      {.artifact_id = std::string{kArtifactId},
       .name = std::string{kArtifactName},
       .description = std::string{kArtifactDescription}});

  EXPECT_EQ(artifact.description(), kArtifactDescription);
  ASSERT_EQ(artifact.parts_size(), 1);
  EXPECT_EQ(artifact.parts(0).raw(), kFilePayload);
  EXPECT_EQ(artifact.parts(0).filename(), kFilename);
  EXPECT_EQ(artifact.parts(0).media_type(), kMediaType);
}

TEST(ResponseBuildersTest, FileUrlArtifactBuildsUrlPartAndFileMetadata) {
  const auto artifact = a2a::core::ResponseBuilders::FileUrlArtifact(
      kFileUrl,
      {.filename = std::string{kFilename}, .media_type = std::string{kMediaType}},
      {.artifact_id = std::string{kArtifactId}, .name = std::string{kArtifactName}});

  ASSERT_EQ(artifact.parts_size(), 1);
  EXPECT_EQ(artifact.parts(0).url(), kFileUrl);
  EXPECT_EQ(artifact.parts(0).filename(), kFilename);
  EXPECT_EQ(artifact.parts(0).media_type(), kMediaType);
}

TEST(ResponseBuildersTest, StructuredDataArtifactBuildsDataPart) {
  google::protobuf::Value structured_data;
  (*structured_data.mutable_struct_value()->mutable_fields())[std::string{kDataField}].set_string_value(
      std::string{kDataValue});

  const auto artifact = a2a::core::ResponseBuilders::StructuredDataArtifact(
      structured_data,
      {.artifact_id = std::string{kArtifactId}, .name = std::string{kArtifactName}},
      {.media_type = std::string{kMediaType}});

  ASSERT_EQ(artifact.parts_size(), 1);
  EXPECT_EQ(artifact.parts(0).data().struct_value().fields().at(std::string{kDataField}).string_value(), kDataValue);
  EXPECT_EQ(artifact.parts(0).media_type(), kMediaType);
}

TEST(ResponseBuildersTest, AddArtifactsWithPrimaryPutsPrimaryFirst) {
  lf::a2a::v1::Task task;
  const auto primary = a2a::core::ResponseBuilders::TextArtifact("primary", {.artifact_id = "p", .name = "p"});
  const auto secondary = a2a::core::ResponseBuilders::TextArtifact("secondary", {.artifact_id = "s", .name = "s"});

  a2a::core::ResponseBuilders::AddArtifactsWithPrimary(&task, primary, {secondary});

  ASSERT_EQ(task.artifacts_size(), 2);
  EXPECT_EQ(task.artifacts(0).artifact_id(), "p");
  EXPECT_EQ(task.artifacts(1).artifact_id(), "s");
}

}  // namespace
