#pragma once

// The .ddjbeat file (BEATMAKER_SPEC §1): the Project as JSON, plus a `samples` array of the user samples it uses.
// Unknown fields - from a newer Devil-DJ - are kept: read into each object's `unknown` and written back on save.

#include "beatmaker/Model.h"
#include "util/Result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace devildj::beatmaker
{

constexpr const char* kFileExtension = ".ddjbeat";

std::string projectToJson(const Project& project);
// `warnings`: what was off but loaded anyway (a newer schema, out-of-range values brought into range, ...).
util::Result<Project> projectFromJson(const std::string& json, std::vector<std::string>* warnings = nullptr);

// On disk. Sample paths inside the file's folder are written relative to it (forward slashes) so a project folder
// can move; on load they are made absolute again. The write goes through a temporary file (no half-written
// project if it fails).
util::Result<bool> saveProject(const Project& project, const std::filesystem::path& file);
util::Result<Project> loadProject(const std::filesystem::path& file, std::vector<std::string>* warnings = nullptr);

} // namespace devildj::beatmaker
