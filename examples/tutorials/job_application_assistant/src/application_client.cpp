#include <iostream>
#include <string>
#include <string_view>

#include "tutorial.h"
int main(int argc, char** argv) {
  std::string url = "http://127.0.0.1:8080";
  std::string resume;
  std::string resume_resource;
  std::string job;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (i + 1 >= argc) {
      std::cerr << "missing option value\n";
      return 2;
    }
    if (arg == "--coordinator-url") {
      url = argv[++i];
    } else if (arg == "--resume-file") {
      resume = argv[++i];
    } else if (arg == "--resume-resource") {
      resume_resource = argv[++i];
    } else if (arg == "--job-file") {
      job = argv[++i];
    } else {
      std::cerr << "unknown option\n";
      return 2;
    }
  }
  if ((resume.empty() == resume_resource.empty()) || job.empty()) {
    std::cerr << "exactly one of --resume-file or --resume-resource, plus --job-file, is required\n";
    return 2;
  }
  auto job_text = job_tutorial::ReadFile(job);
  auto resume_text = resume.empty() ? a2a::core::Result<std::string>(std::string{}) : job_tutorial::ReadFile(resume);
  if (!resume_text.ok() || !job_text.ok()) {
    std::cerr << (!resume_text.ok() ? resume_text.error().message() : job_text.error().message()) << '\n';
    return 1;
  }
  const auto request = resume_resource.empty() ? job_tutorial::JobRequest(resume_text.value(), job_text.value())
                                               : job_tutorial::JobResourceRequest(resume_resource, job_text.value());
  auto response = job_tutorial::Send(url, request);
  if (!response.ok()) {
    std::cerr << response.error().message() << '\n';
    return 1;
  }
  std::cout << job_tutorial::Render(response.value());
  return 0;
}
