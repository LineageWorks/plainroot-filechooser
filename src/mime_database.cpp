#include "mime_database.hpp"

#include <glib.h>
#include <magic.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fnmatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace plainroot::filechooser::detail {
namespace {

std::vector<std::string> default_mime_directories() {
  std::vector<std::string> directories;
  const gchar *const *system_directories = g_get_system_data_dirs();
  if (system_directories != nullptr) {
    std::vector<std::string> ordered;
    for (size_t index = 0; system_directories[index] != nullptr; ++index)
      ordered.emplace_back(system_directories[index]);
    for (auto iterator = ordered.rbegin(); iterator != ordered.rend();
         ++iterator)
      directories.push_back(*iterator + "/mime");
  }
  const gchar *user_directory = g_get_user_data_dir();
  if (user_directory != nullptr)
    directories.emplace_back(std::string(user_directory) + "/mime");
  return directories;
}

std::string ascii_lower(std::string_view value) {
  gchar *lower = g_ascii_strdown(value.data(), static_cast<gssize>(value.size()));
  if (lower == nullptr)
    return {};
  std::string result(lower);
  g_free(lower);
  return result;
}

std::string unicode_casefold(std::string_view value) {
  if (!g_utf8_validate(value.data(), static_cast<gssize>(value.size()),
                       nullptr))
    return ascii_lower(value);
  gchar *folded = g_utf8_casefold(value.data(), static_cast<gssize>(value.size()));
  if (folded == nullptr)
    return ascii_lower(value);
  std::string result(folded);
  g_free(folded);
  return result;
}

bool has_glob_metacharacter(std::string_view pattern) {
  return pattern.find_first_of("*?[") != std::string_view::npos;
}

} // namespace

bool mime_type_pattern_matches(const std::vector<std::string> &types,
                               std::string_view requested_type) {
  if (requested_type.empty() || types.empty())
    return false;
  const std::string requested = ascii_lower(requested_type);
  if (requested == "*/*")
    return true;

  const size_t slash = requested.find('/');
  if (slash != std::string::npos && requested.substr(slash + 1U) == "*") {
    const std::string requested_media = requested.substr(0, slash);
    return std::any_of(types.begin(), types.end(), [&requested_media](
                                                     const std::string &type) {
      const size_t type_slash = type.find('/');
      return type_slash != std::string::npos &&
             ascii_lower(std::string_view(type).substr(0, type_slash)) ==
                 requested_media;
    });
  }

  return std::any_of(types.begin(), types.end(), [&requested](
                                                 const std::string &type) {
    return ascii_lower(type) == requested;
  });
}

class MimeDatabase::Impl {
public:
  struct GlobRule {
    std::string type;
    std::string pattern;
    std::string folded_pattern;
    int weight = 0;
    bool case_sensitive = false;
  };

  explicit Impl(std::vector<std::string> mime_directories,
                bool enable_content_sniffing) {
    for (const std::string &directory : mime_directories)
      load_directory(directory);
    if (enable_content_sniffing) {
      magic_ = magic_open(MAGIC_MIME_TYPE | MAGIC_NO_COMPRESS_FORK |
                          MAGIC_NO_CHECK_COMPRESS | MAGIC_PRESERVE_ATIME);
      if (magic_ != nullptr && magic_load(magic_, nullptr) != 0) {
        magic_close(magic_);
        magic_ = nullptr;
      }
    }
  }

  ~Impl() {
    if (magic_ != nullptr)
      magic_close(magic_);
  }

  bool available() const noexcept {
    return !globs_.empty() || magic_ != nullptr;
  }

  bool content_sniffing_available() const noexcept { return magic_ != nullptr; }

  std::vector<std::string> types_for_file(std::string_view native_path,
                                          std::string_view native_name) {
    auto types = matching_globs(native_name);
    if (types.size() == 1U)
      return include_parent_types(types);

    const std::string sniffed = sniff_file(native_path);
    if (!sniffed.empty())
      return include_parent_types({canonical_type(sniffed)});
    return include_parent_types({fallback_type(native_path)});
  }

  std::vector<std::string>
  include_parent_types(const std::vector<std::string> &types) const {
    std::vector<std::string> expanded;
    std::vector<std::string> pending;
    for (const std::string &type : types)
      pending.push_back(canonical_type(type));

    while (!pending.empty()) {
      std::string current = std::move(pending.back());
      pending.pop_back();
      if (std::find(expanded.begin(), expanded.end(), current) !=
          expanded.end())
        continue;
      expanded.push_back(current);

      for (const auto &[alias, canonical] : aliases_) {
        if (canonical_type(canonical) == current &&
            std::find(expanded.begin(), expanded.end(), alias) ==
                expanded.end())
          expanded.push_back(alias);
      }

      const auto parents = parents_.find(current);
      if (parents == parents_.end())
        continue;
      for (const std::string &parent : parents->second)
        pending.push_back(canonical_type(parent));
    }
    return expanded;
  }

  bool matches(const std::vector<std::string> &types,
               std::string_view requested_type) const {
    return mime_type_pattern_matches(include_parent_types(types),
                                     canonical_type(requested_type));
  }

private:
  void load_directory(const std::string &directory) {
    load_globs(directory + "/globs2");
    load_aliases(directory + "/aliases");
    load_subclasses(directory + "/subclasses");
  }

  void load_globs(const std::string &path) {
    std::vector<GlobRule> directory_globs;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line.front() == '#')
        continue;
      const size_t first = line.find(':');
      const size_t second = first == std::string::npos
                                ? std::string::npos
                                : line.find(':', first + 1U);
      if (first == std::string::npos || second == std::string::npos)
        continue;
      int weight = 0;
      const auto parsed = std::from_chars(line.data(),
                                          line.data() + first, weight);
      if (parsed.ec != std::errc{} || parsed.ptr != line.data() + first ||
          weight < 0 || weight > 100)
        continue;
      const std::string type = line.substr(first + 1U, second - first - 1U);
      const size_t third = line.find(':', second + 1U);
      const std::string pattern = line.substr(
          second + 1U, third == std::string::npos
                           ? std::string::npos
                           : third - second - 1U);
      if (type.empty() || pattern.empty())
        continue;
      if (pattern == "__NOGLOBS__") {
        std::erase_if(globs_, [&type](const GlobRule &rule) {
          return rule.type == type;
        });
        std::erase_if(directory_globs, [&type](const GlobRule &rule) {
          return rule.type == type;
        });
        continue;
      }
      const size_t fourth = third == std::string::npos
                                ? std::string::npos
                                : line.find(':', third + 1U);
      const std::string flags = third == std::string::npos
                                    ? std::string{}
                                    : line.substr(third + 1U,
                                                  fourth == std::string::npos
                                                      ? std::string::npos
                                                      : fourth - third - 1U);
      const bool case_sensitive =
          flags == "cs" || flags.starts_with("cs,") ||
          flags.find(",cs") != std::string::npos;
      directory_globs.push_back(
          {type, pattern, unicode_casefold(pattern), weight, case_sensitive});
    }

    std::unordered_set<std::string> replaced_patterns;
    for (const GlobRule &rule : directory_globs)
      replaced_patterns.insert(rule.pattern);
    std::erase_if(globs_, [&replaced_patterns](const GlobRule &rule) {
      return replaced_patterns.contains(rule.pattern);
    });
    globs_.insert(globs_.end(), directory_globs.begin(), directory_globs.end());
  }

  void load_aliases(const std::string &path) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line.front() == '#')
        continue;
      std::istringstream fields(line);
      std::string alias;
      std::string canonical;
      if (fields >> alias >> canonical)
        aliases_[std::move(alias)] = std::move(canonical);
    }
  }

  void load_subclasses(const std::string &path) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line.front() == '#')
        continue;
      std::istringstream fields(line);
      std::string child;
      std::string parent;
      if (fields >> child >> parent)
        parents_[std::move(child)].push_back(std::move(parent));
    }
  }

  std::string canonical_type(std::string_view type) const {
    std::string canonical(type);
    for (size_t depth = 0; depth < 16U; ++depth) {
      const auto found = aliases_.find(canonical);
      if (found == aliases_.end() || found->second == canonical)
        break;
      canonical = found->second;
    }
    return canonical;
  }

  std::vector<std::string>
  matching_globs(std::string_view native_name) const {
    std::vector<const GlobRule *> matches;
    const std::string folded_name = unicode_casefold(native_name);
    for (const GlobRule &rule : globs_) {
      if (rule.pattern == "__NOGLOBS__")
        continue;
      const std::string candidate = rule.case_sensitive
                                        ? std::string(native_name)
                                        : folded_name;
      const std::string pattern = rule.case_sensitive
                                      ? rule.pattern
                                      : rule.folded_pattern;
      if (fnmatch(pattern.c_str(), candidate.c_str(), 0) == 0)
        matches.push_back(&rule);
    }
    if (matches.empty())
      return {};

    const bool has_literal = std::any_of(
        matches.begin(), matches.end(), [](const GlobRule *rule) {
          return !has_glob_metacharacter(rule->pattern);
        });
    if (has_literal)
      std::erase_if(matches, [](const GlobRule *rule) {
        return has_glob_metacharacter(rule->pattern);
      });
    const int best_weight = (*std::max_element(
        matches.begin(), matches.end(),
        [](const GlobRule *left, const GlobRule *right) {
          return left->weight < right->weight;
        }))->weight;
    std::erase_if(matches, [best_weight](const GlobRule *rule) {
      return rule->weight != best_weight;
    });
    const size_t best_length = (*std::max_element(
        matches.begin(), matches.end(),
        [](const GlobRule *left, const GlobRule *right) {
          return left->pattern.size() < right->pattern.size();
        }))->pattern.size();
    std::erase_if(matches, [best_length](const GlobRule *rule) {
      return rule->pattern.size() != best_length;
    });

    std::vector<std::string> types;
    for (const GlobRule *rule : matches) {
      const std::string canonical = canonical_type(rule->type);
      if (std::find(types.begin(), types.end(), canonical) == types.end())
        types.push_back(canonical);
    }
    return types;
  }

  std::string sniff_file(std::string_view path) {
    if (magic_ == nullptr || path.empty() || path.find('\0') != std::string_view::npos)
      return {};
    const std::string native_path(path);
    const int descriptor =
        open(native_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0)
      return {};
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
      close(descriptor);
      return {};
    }
    const char *type = magic_descriptor(magic_, descriptor);
    const std::string result = type == nullptr ? std::string{} : std::string(type);
    close(descriptor);
    return result;
  }

  static std::string fallback_type(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos)
      return "application/octet-stream";
    const std::string native_path(path);
    const int descriptor =
        open(native_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0)
      return "application/octet-stream";
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
      close(descriptor);
      return "application/octet-stream";
    }
    std::array<unsigned char, 128> prefix{};
    const ssize_t count = read(descriptor, prefix.data(), prefix.size());
    close(descriptor);
    if (count < 0)
      return "application/octet-stream";
    for (ssize_t index = 0; index < count; ++index) {
      const unsigned char value = prefix[static_cast<size_t>(index)];
      if ((value < 0x20U && value != '\t' && value != '\n' && value != '\f' &&
           value != '\r') ||
          value == 0x7fU)
        return "application/octet-stream";
    }
    return "text/plain";
  }

  std::vector<GlobRule> globs_;
  std::unordered_map<std::string, std::string> aliases_;
  std::unordered_map<std::string, std::vector<std::string>> parents_;
  magic_t magic_ = nullptr;
};

MimeDatabase::MimeDatabase()
    : MimeDatabase(default_mime_directories(), true) {}

MimeDatabase::MimeDatabase(std::vector<std::string> mime_directories,
                           bool enable_content_sniffing)
    : impl_(std::make_unique<Impl>(std::move(mime_directories),
                                   enable_content_sniffing)) {}

MimeDatabase::MimeDatabase(MimeDatabase &&) noexcept = default;
MimeDatabase &MimeDatabase::operator=(MimeDatabase &&) noexcept = default;
MimeDatabase::~MimeDatabase() = default;

bool MimeDatabase::available() const noexcept {
  return impl_ != nullptr && impl_->available();
}

bool MimeDatabase::content_sniffing_available() const noexcept {
  return impl_ != nullptr && impl_->content_sniffing_available();
}

std::vector<std::string>
MimeDatabase::types_for_file(std::string_view native_path,
                             std::string_view native_name) {
  return impl_ == nullptr ? std::vector<std::string>{}
                          : impl_->types_for_file(native_path, native_name);
}

std::vector<std::string> MimeDatabase::include_parent_types(
    const std::vector<std::string> &types) const {
  return impl_ == nullptr ? std::vector<std::string>{}
                          : impl_->include_parent_types(types);
}

bool MimeDatabase::matches(const std::vector<std::string> &types,
                           std::string_view requested_type) const {
  return impl_ != nullptr && impl_->matches(types, requested_type);
}

} // namespace plainroot::filechooser::detail
