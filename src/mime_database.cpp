#include "mime_database.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

constexpr std::string_view kMagicHeader("MIME-Magic\0\n", 12);
constexpr size_t kMaximumMagicDatabaseSize = 32U * 1024U * 1024U;
constexpr size_t kMaximumSniffExtent = 16U * 1024U * 1024U;
constexpr uint64_t kMaximumMagicRange = 1024U * 1024U;

struct MagicMatchlet {
  size_t indent = 0;
  size_t offset = 0;
  size_t range_length = 1;
  size_t word_size = 1;
  std::vector<unsigned char> value;
  std::vector<unsigned char> mask;
};

struct MagicRule {
  std::string type;
  int priority = 0;
  size_t directory_rank = 0;
  std::vector<MagicMatchlet> matchlets;
};

bool read_magic_number(std::string_view bytes, size_t &position,
                       uint64_t &number) {
  const size_t start = position;
  while (position < bytes.size() &&
         std::isdigit(static_cast<unsigned char>(bytes[position])))
    ++position;
  if (position == start)
    return false;
  const auto parsed = std::from_chars(bytes.data() + start,
                                      bytes.data() + position, number);
  return parsed.ec == std::errc{} && parsed.ptr == bytes.data() + position;
}

bool parse_magic_rule(std::string_view bytes, size_t &position,
                      MagicMatchlet &matchlet) {
  uint64_t number = 0;
  if (position >= bytes.size() || bytes[position] == '[')
    return false;
  if (bytes[position] == '\n') {
    ++position;
    return false;
  }

  if (std::isdigit(static_cast<unsigned char>(bytes[position]))) {
    if (!read_magic_number(bytes, position, number) ||
        number > std::numeric_limits<size_t>::max())
      return false;
    matchlet.indent = static_cast<size_t>(number);
  } else {
    matchlet.indent = 0;
  }
  if (position >= bytes.size() || bytes[position++] != '>')
    return false;
  if (!read_magic_number(bytes, position, number) ||
      number > kMaximumSniffExtent)
    return false;
  matchlet.offset = static_cast<size_t>(number);
  if (position >= bytes.size() || bytes[position++] != '=')
    return false;
  if (position + 2U > bytes.size())
    return false;
  const size_t value_length =
      (static_cast<size_t>(static_cast<unsigned char>(bytes[position])) << 8U) |
      static_cast<size_t>(static_cast<unsigned char>(bytes[position + 1U]));
  position += 2U;
  if (value_length == 0U || position + value_length > bytes.size())
    return false;
  matchlet.value.reserve(value_length);
  for (size_t index = 0; index < value_length; ++index)
    matchlet.value.push_back(
        static_cast<unsigned char>(bytes[position + index]));
  position += value_length;

  if (position < bytes.size() && bytes[position] == '&') {
    ++position;
    if (position + value_length > bytes.size())
      return false;
    matchlet.mask.reserve(value_length);
    for (size_t index = 0; index < value_length; ++index)
      matchlet.mask.push_back(
          static_cast<unsigned char>(bytes[position + index]));
    position += value_length;
  }
  if (position < bytes.size() && bytes[position] == '~') {
    ++position;
    if (!read_magic_number(bytes, position, number) ||
        (number != 0U && number != 1U && number != 2U && number != 4U))
      return false;
    matchlet.word_size = static_cast<size_t>(number);
  }
  if (position < bytes.size() && bytes[position] == '+') {
    ++position;
    if (!read_magic_number(bytes, position, number) ||
        number > kMaximumMagicRange)
      return false;
    matchlet.range_length = static_cast<size_t>(number);
  }
  if (position >= bytes.size() || bytes[position] != '\n') {
    // The XDG format reserves unknown suffix characters for future extensions.
    // Their line has no further binary data, so skip it without interpreting it.
    while (position < bytes.size() && bytes[position] != '\n')
      ++position;
    if (position < bytes.size())
      ++position;
    return false;
  }
  ++position;

  if (matchlet.range_length == 0U)
    return false;
  if (matchlet.word_size > 1U) {
    if (value_length % matchlet.word_size != 0U)
      return false;
    if (std::endian::native == std::endian::little) {
      for (size_t offset = 0; offset < value_length;
           offset += matchlet.word_size) {
        std::reverse(matchlet.value.begin() +
                         static_cast<std::ptrdiff_t>(offset),
                     matchlet.value.begin() + static_cast<std::ptrdiff_t>(
                                                  offset + matchlet.word_size));
        if (!matchlet.mask.empty())
          std::reverse(matchlet.mask.begin() +
                           static_cast<std::ptrdiff_t>(offset),
                       matchlet.mask.begin() + static_cast<std::ptrdiff_t>(
                                                   offset + matchlet.word_size));
      }
    }
  }
  if (matchlet.offset > kMaximumSniffExtent ||
      matchlet.range_length > kMaximumSniffExtent ||
      matchlet.offset + matchlet.range_length - 1U > kMaximumSniffExtent ||
      value_length > kMaximumSniffExtent - matchlet.offset -
                         (matchlet.range_length - 1U))
    return false;
  return true;
}

bool magic_matchlet_matches(const MagicMatchlet &matchlet,
                            const std::vector<unsigned char> &data) {
  for (size_t offset = 0; offset < matchlet.range_length; ++offset) {
    const size_t candidate_offset = matchlet.offset + offset;
    if (candidate_offset > data.size() ||
        matchlet.value.size() > data.size() - candidate_offset)
      return false;
    bool matches = true;
    for (size_t index = 0; index < matchlet.value.size(); ++index) {
      const unsigned char mask = matchlet.mask.empty()
                                     ? 0xffU
                                     : matchlet.mask[index];
      if ((matchlet.value[index] & mask) !=
          (data[candidate_offset + index] & mask)) {
        matches = false;
        break;
      }
    }
    if (matches)
      return true;
  }
  return false;
}

bool magic_rule_level_matches(const MagicRule &rule,
                              const std::vector<unsigned char> &data,
                              size_t &position, size_t indent) {
  bool level_matches = false;
  while (position < rule.matchlets.size() &&
         rule.matchlets[position].indent == indent) {
    const MagicMatchlet &matchlet = rule.matchlets[position++];
    const bool parent_matches = magic_matchlet_matches(matchlet, data);
    const bool has_children = position < rule.matchlets.size() &&
                              rule.matchlets[position].indent > indent;
    bool children_match = true;
    if (has_children && parent_matches) {
      children_match = magic_rule_level_matches(rule, data, position,
                                                indent + 1U);
    } else if (has_children) {
      while (position < rule.matchlets.size() &&
             rule.matchlets[position].indent > indent)
        ++position;
    }
    if (parent_matches && (!has_children || children_match))
      level_matches = true;
  }
  return level_matches;
}

bool magic_rule_matches(const MagicRule &rule,
                        const std::vector<unsigned char> &data) {
  if (rule.matchlets.empty() || rule.matchlets.front().indent != 0U)
    return false;
  size_t position = 0;
  return magic_rule_level_matches(rule, data, position, 0U);
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
    for (size_t rank = 0; rank < mime_directories.size(); ++rank)
      load_directory(mime_directories[rank], rank, enable_content_sniffing);
  }

  bool available() const noexcept {
    return !globs_.empty() || !magic_rules_.empty();
  }

  bool content_sniffing_available() const noexcept {
    return !magic_rules_.empty();
  }

  std::vector<std::string> types_for_file(std::string_view native_path,
                                          std::string_view native_name) {
    const auto candidates = matching_globs(native_name);
    const std::string sniffed = sniff_file(native_path);
    if (sniffed.empty()) {
      if (!candidates.empty())
        return include_parent_types({candidates.front()});
      return include_parent_types({fallback_type(native_path)});
    }

    if (candidates.empty())
      return include_parent_types({sniffed});

    const auto sniffed_hierarchy = include_parent_types({sniffed});
    for (const std::string &candidate : candidates) {
      const std::string canonical_candidate = canonical_type(candidate);
      if (canonical_candidate == sniffed ||
          std::find(sniffed_hierarchy.begin(), sniffed_hierarchy.end(),
                    canonical_candidate) != sniffed_hierarchy.end())
        return include_parent_types({sniffed});
    }

    // When the content rules do not refine a filename candidate, retain the
    // highest-ranked XDG glob candidate, as the shared MIME matcher does.
    return include_parent_types({candidates.front()});
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
  void load_directory(const std::string &directory, size_t rank,
                      bool enable_content_sniffing) {
    load_globs(directory + "/globs2");
    load_aliases(directory + "/aliases");
    load_subclasses(directory + "/subclasses");
    if (enable_content_sniffing)
      load_magic(directory + "/magic", rank);
  }

  void load_magic(const std::string &path, size_t directory_rank) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
      return;
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < static_cast<std::streamoff>(kMagicHeader.size()) ||
        length > static_cast<std::streamoff>(kMaximumMagicDatabaseSize))
      return;
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(length), '\0');
    input.read(bytes.data(), length);
    if (!input || !std::string_view(bytes).starts_with(kMagicHeader))
      return;

    size_t position = kMagicHeader.size();
    MagicRule section;
    bool have_section = false;
    bool last_matchlet_valid = true;
    auto finish_section = [&]() {
      if (have_section && !section.matchlets.empty()) {
        for (const MagicMatchlet &matchlet : section.matchlets) {
          const size_t extent = matchlet.offset + matchlet.value.size() +
                                matchlet.range_length - 1U;
          magic_max_extent_ = std::max(magic_max_extent_, extent);
        }
        magic_rules_.push_back(std::move(section));
      }
      section = {};
      have_section = false;
      last_matchlet_valid = true;
    };

    while (position < bytes.size()) {
      if (bytes[position] == '[') {
        finish_section();
        const size_t line_end = bytes.find('\n', position);
        if (line_end == std::string::npos)
          break;
        const std::string_view header(bytes.data() + position + 1U,
                                      line_end - position - 1U);
        const size_t colon = header.find(':');
        if (colon == std::string_view::npos || header.empty() ||
            header.back() != ']') {
          position = line_end + 1U;
          have_section = false;
          continue;
        }
        int priority = 0;
        const auto parsed_priority = std::from_chars(
            header.data(), header.data() + colon, priority);
        const std::string_view type = header.substr(
            colon + 1U, header.size() - colon - 2U);
        if (parsed_priority.ec != std::errc{} ||
            parsed_priority.ptr != header.data() + colon || type.empty()) {
          position = line_end + 1U;
          have_section = false;
          continue;
        }
        section.type.assign(type);
        section.priority = priority;
        section.directory_rank = directory_rank;
        have_section = true;
        position = line_end + 1U;
        continue;
      }
      if (!have_section) {
        const size_t line_end = bytes.find('\n', position);
        position = line_end == std::string::npos ? bytes.size() : line_end + 1U;
        continue;
      }
      if (bytes[position] == '\n') {
        ++position;
        continue;
      }

      const size_t line_start = position;
      MagicMatchlet matchlet;
      if (!parse_magic_rule(bytes, position, matchlet)) {
        if (position == line_start) {
          const size_t line_end = bytes.find('\n', position);
          position = line_end == std::string::npos ? bytes.size()
                                                   : line_end + 1U;
        }
        last_matchlet_valid = false;
        continue;
      }
      if ((!section.matchlets.empty() &&
           matchlet.indent > section.matchlets.back().indent + 1U) ||
          (section.matchlets.empty() && matchlet.indent != 0U)) {
        last_matchlet_valid = false;
        continue;
      }

      const std::string marker(matchlet.value.begin(), matchlet.value.end());
      if (matchlet.indent == 0U && matchlet.offset == 0U &&
          matchlet.range_length == 1U && matchlet.mask.empty() &&
          marker == "__NOMAGIC__") {
        const std::string replaced_type = ascii_lower(section.type);
        std::erase_if(magic_rules_, [&replaced_type](const MagicRule &rule) {
          return ascii_lower(rule.type) == replaced_type;
        });
        section.matchlets.clear();
        last_matchlet_valid = true;
        continue;
      }
      if (!last_matchlet_valid && matchlet.indent != 0U)
        continue;
      section.matchlets.push_back(std::move(matchlet));
      last_matchlet_valid = true;
    }
    finish_section();
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

  std::string sniff_file(std::string_view path) const {
    if (magic_rules_.empty() || magic_max_extent_ == 0U || path.empty() ||
        path.find('\0') != std::string_view::npos)
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
    const size_t file_extent =
        status.st_size < 0
            ? magic_max_extent_
            : std::min(magic_max_extent_, static_cast<size_t>(status.st_size));
    std::vector<unsigned char> data(file_extent);
    size_t bytes_read = 0;
    while (bytes_read < data.size()) {
      const ssize_t count =
          read(descriptor, data.data() + bytes_read, data.size() - bytes_read);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        break;
      bytes_read += static_cast<size_t>(count);
    }
    close(descriptor);
    data.resize(bytes_read);

    const MagicRule *best = nullptr;
    for (const MagicRule &rule : magic_rules_) {
      if (best != nullptr && rule.priority < best->priority)
        continue;
      if (!magic_rule_matches(rule, data))
        continue;
      if (best == nullptr || rule.priority > best->priority ||
          (rule.priority == best->priority &&
           rule.directory_rank > best->directory_rank))
        best = &rule;
    }
    return best == nullptr ? std::string{} : canonical_type(best->type);
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
  std::vector<MagicRule> magic_rules_;
  size_t magic_max_extent_ = 0;
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
