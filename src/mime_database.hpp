#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plainroot::filechooser::detail {

bool mime_type_pattern_matches(const std::vector<std::string> &types,
                               std::string_view requested_type);

class MimeDatabase {
public:
  MimeDatabase();
  explicit MimeDatabase(std::vector<std::string> mime_directories,
                        bool enable_content_sniffing = true);
  MimeDatabase(const MimeDatabase &) = delete;
  MimeDatabase &operator=(const MimeDatabase &) = delete;
  MimeDatabase(MimeDatabase &&) noexcept;
  MimeDatabase &operator=(MimeDatabase &&) noexcept;
  ~MimeDatabase();

  bool available() const noexcept;
  bool content_sniffing_available() const noexcept;
  std::vector<std::string> types_for_file(std::string_view native_path,
                                          std::string_view native_name);
  std::vector<std::string>
  include_parent_types(const std::vector<std::string> &types) const;
  bool matches(const std::vector<std::string> &types,
               std::string_view requested_type) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser::detail
