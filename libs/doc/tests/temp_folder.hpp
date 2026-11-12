// SPDX-License-Identifier: Apache-2.0

// A folder of a test case's own, for doc tests that write files.

#pragma once

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace cs::doc::testing {

// The folder `doc_test_scratch/<name>` under the working directory, which CTest sets to the
// module's build folder: two presets or two worktrees testing at once never share it, and CTest's
// parallel test processes do not either as long as every test case passes a name of its own.
// Removed before and after; whoever needs the folder itself creates it, so a test can also use
// it as a folder that does not exist yet. The name is snake_case (a-z, 0-9, _), so that removing
// the folder never reaches outside doc_test_scratch/.
class TempFolder {
  public:
    explicit TempFolder(std::string_view name)
        : path_(std::filesystem::current_path() / "doc_test_scratch" / checked(name)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_.parent_path());
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    static std::string_view checked(std::string_view name) {
        const bool snake_case = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
        if (!snake_case) {
            throw std::invalid_argument("TempFolder: \"" + std::string(name) +
                                        "\" is not a snake_case folder name");
        }
        return name;
    }

    std::filesystem::path path_;
};

} // namespace cs::doc::testing
