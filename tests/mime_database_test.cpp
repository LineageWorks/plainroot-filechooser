#include "../src/mime_database.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

using plainroot::filechooser::detail::MimeDatabase;

namespace {

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::ofstream output(path, std::ios::binary);
  assert(output.good());
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  assert(output.good());
}

void append_magic_section(std::string &magic, int priority,
                          const std::string &type) {
  magic += "[" + std::to_string(priority) + ":" + type + "]\n";
}

void append_magic_rule(std::string &magic, size_t indent, size_t offset,
                       const std::string &value,
                       const std::string &mask = {}, size_t word_size = 1,
                       size_t range_length = 1) {
  if (indent != 0U)
    magic += std::to_string(indent);
  magic += ">" + std::to_string(offset) + "=";
  magic.push_back(static_cast<char>((value.size() >> 8U) & 0xffU));
  magic.push_back(static_cast<char>(value.size() & 0xffU));
  magic += value;
  if (!mask.empty()) {
    magic.push_back('&');
    magic += mask;
  }
  if (word_size != 1U)
    magic += "~" + std::to_string(word_size);
  if (range_length != 1U)
    magic += "+" + std::to_string(range_length);
  magic.push_back('\n');
}

bool contains(const std::vector<std::string> &values,
              const std::string &value) {
  for (const std::string &candidate : values) {
    if (candidate == value)
      return true;
  }
  return false;
}

} // namespace

int main() {
  char temporary_directory[] = "/tmp/plainroot-filechooser-mime-XXXXXX";
  const char *created = mkdtemp(temporary_directory);
  assert(created != nullptr);
  const std::filesystem::path root(created);
  const std::filesystem::path low = root / "low" / "mime";
  const std::filesystem::path high = root / "high" / "mime";
  std::filesystem::create_directories(low);
  std::filesystem::create_directories(high);

  write_text(low / "globs2",
             "50:text/plain:*.txt\n"
             "50:application/gzip:*.gz\n"
             "50:application/x-long-archive:*.tar.gz\n"
             "50:application/x-ambiguous-one:*.amb\n"
             "50:application/x-ambiguous-two:*.amb\n"
             "60:application/x-weighted:*.wgt\n"
             "50:application/x-long-weighted:*.long.wgt\n"
             "50:text/x-code:*.src:cs\n"
             "50:text/plain:Makefile\n"
             "50:application/x-old:*.old\n");
  write_text(high / "globs2",
             "40:application/x-user:*.txt\n"
             "0:application/x-old:__NOGLOBS__\n"
             "50:application/x-new:*.old\n");
  write_text(high / "aliases", "application/x-user-alias application/x-user\n");
  write_text(high / "subclasses", "text/x-code text/plain\n");

  const auto text_file = root / "document.txt";
  const auto archive_file = root / "archive.tar.gz";
  const auto ambiguous_file = root / "sample.amb";
  const auto code_file = root / "source.src";
  const auto upper_code_file = root / "source.SRC";
  const auto old_file = root / "legacy.old";
  for (const auto &file : {text_file, archive_file, ambiguous_file, code_file,
                           upper_code_file, old_file})
    write_text(file, "plain text content\n");

  MimeDatabase database({low.string(), high.string()}, false);
  assert(database.available());
  assert(!database.content_sniffing_available());

  const auto text_types =
      database.types_for_file(text_file.string(), "DOCUMENT.TXT");
  assert(contains(text_types, "application/x-user"));
  assert(contains(text_types, "application/x-user-alias"));
  assert(database.matches(text_types, "application/x-user-alias"));
  assert(database.matches(text_types, "APPLICATION/*"));

  const auto archive_types =
      database.types_for_file(archive_file.string(), "archive.tar.gz");
  assert(contains(archive_types, "application/x-long-archive"));
  assert(!contains(archive_types, "application/gzip"));

  const auto weighted_types =
      database.types_for_file(text_file.string(), "report.long.wgt");
  assert(contains(weighted_types, "application/x-weighted"));
  assert(!contains(weighted_types, "application/x-long-weighted"));

  const auto ambiguous_types =
      database.types_for_file(ambiguous_file.string(), "sample.amb");
  assert(contains(ambiguous_types, "application/x-ambiguous-one"));
  assert(!contains(ambiguous_types, "text/plain"));
  MimeDatabase sniffing_database({low.string(), high.string()}, true);
  if (sniffing_database.content_sniffing_available()) {
    const auto sniffed_ambiguous_types = sniffing_database.types_for_file(
        ambiguous_file.string(), "sample.amb");
    assert(contains(sniffed_ambiguous_types, "application/x-ambiguous-one"));
    assert(!contains(sniffed_ambiguous_types, "text/plain"));
  }

  const auto code_types =
      database.types_for_file(code_file.string(), "source.src");
  assert(contains(code_types, "text/x-code"));
  assert(contains(code_types, "text/plain"));
  assert(database.matches(code_types, "text/*"));
  const auto upper_code_types =
      database.types_for_file(upper_code_file.string(), "source.SRC");
  assert(!contains(upper_code_types, "text/x-code"));

  const auto old_types =
      database.types_for_file(old_file.string(), "legacy.old");
  assert(contains(old_types, "application/x-new"));
  assert(!contains(old_types, "application/x-old"));

  MimeDatabase system_database;
  const auto unknown_file = root / "signature.unknown";
  {
    const unsigned char png_header[] = {
        0x89, 'P',  'N',  'G',  '\r', '\n', 0x1a, '\n',
        0,    0,    0,    13,   'I',  'H',  'D',  'R',
        0,    0,    0,    1,    0,    0,    0,    1,
        8,    6,    0,    0,    0};
    std::ofstream output(unknown_file, std::ios::binary);
    assert(output.good());
    output.write(reinterpret_cast<const char *>(png_header),
                 static_cast<std::streamsize>(sizeof(png_header)));
    assert(output.good());
  }
  if (system_database.content_sniffing_available()) {
    const auto image_types =
        system_database.types_for_file(unknown_file.string(), "signature.unknown");
    assert(contains(image_types, "image/png"));
    assert(system_database.matches(image_types, "image/*"));
  }

  const std::filesystem::path magic_low = root / "magic-low" / "mime";
  const std::filesystem::path magic_high = root / "magic-high" / "mime";
  std::filesystem::create_directories(magic_low);
  std::filesystem::create_directories(magic_high);
  write_text(magic_low / "globs2",
             "50:audio/ogg:*.ogg\n"
             "50:video/ogg:*.ogg\n"
             "50:audio/x-vorbis+ogg:*.ogg\n"
             "50:audio/x-flac+ogg:*.ogg\n");
  write_text(magic_low / "subclasses",
             "audio/x-vorbis+ogg audio/ogg\n");

  std::string low_magic("MIME-Magic\0\n", 12);
  append_magic_section(low_magic, 80, "application/x-signature");
  append_magic_rule(low_magic, 0, 0, "SIG!");
  append_magic_section(low_magic, 70, "application/x-nested");
  append_magic_rule(low_magic, 0, 0, "ROOT");
  append_magic_rule(low_magic, 1, 4, "GOOD");
  append_magic_rule(low_magic, 0, 0, "ALTN");
  append_magic_section(low_magic, 90, "application/x-masked");
  append_magic_rule(low_magic, 0, 0, std::string(1, '\xa0'),
                    std::string(1, '\xf0'));
  append_magic_section(low_magic, 70, "application/x-ranged");
  append_magic_rule(low_magic, 0, 0, "Z", {}, 1, 4);
  append_magic_section(low_magic, 70, "application/x-host-word");
  append_magic_rule(low_magic, 0, 0, std::string("\x01\x02\x03\x04", 4),
                    {}, 4);
  append_magic_section(low_magic, 30, "application/x-priority-low");
  append_magic_rule(low_magic, 0, 0, "PRIO");
  append_magic_section(low_magic, 90, "application/x-priority-high");
  append_magic_rule(low_magic, 0, 0, "PRIO");
  append_magic_section(low_magic, 80, "audio/x-vorbis+ogg");
  append_magic_rule(low_magic, 0, 0, "CODEC");
  append_magic_section(low_magic, 80, "application/x-overridden");
  append_magic_rule(low_magic, 0, 0, "OLD");
  write_text(magic_low / "magic", low_magic);

  std::string high_magic("MIME-Magic\0\n", 12);
  append_magic_section(high_magic, 80, "application/x-overridden");
  append_magic_rule(high_magic, 0, 0, "__NOMAGIC__");
  append_magic_rule(high_magic, 0, 0, "NEW");
  write_text(magic_high / "magic", high_magic);

  MimeDatabase xdg_magic_database({magic_low.string(), magic_high.string()});
  assert(xdg_magic_database.content_sniffing_available());
  const auto signature_file = root / "signature-data.unknown";
  write_text(signature_file, "SIG!");
  assert(contains(xdg_magic_database.types_for_file(
                      signature_file.string(), signature_file.filename().string()),
                  "application/x-signature"));

  const auto nested_file = root / "nested.unknown";
  write_text(nested_file, "ROOTGOOD");
  auto nested_types = xdg_magic_database.types_for_file(
      nested_file.string(), nested_file.filename().string());
  assert(contains(nested_types, "application/x-nested"));
  write_text(nested_file, "ROOTBAD!");
  nested_types = xdg_magic_database.types_for_file(
      nested_file.string(), nested_file.filename().string());
  assert(!contains(nested_types, "application/x-nested"));
  write_text(nested_file, "ALTN");
  nested_types = xdg_magic_database.types_for_file(
      nested_file.string(), nested_file.filename().string());
  assert(contains(nested_types, "application/x-nested"));
  write_text(nested_file, "ALTNGOOD");
  nested_types = xdg_magic_database.types_for_file(
      nested_file.string(), nested_file.filename().string());
  assert(contains(nested_types, "application/x-nested"));

  const auto masked_file = root / "masked.unknown";
  write_text(masked_file, std::string(1, '\xaf'));
  assert(contains(xdg_magic_database.types_for_file(
                      masked_file.string(), masked_file.filename().string()),
                  "application/x-masked"));
  const auto ranged_file = root / "ranged.unknown";
  write_text(ranged_file, "abcZ");
  assert(contains(xdg_magic_database.types_for_file(
                      ranged_file.string(), ranged_file.filename().string()),
                  "application/x-ranged"));
  const auto word_file = root / "word.unknown";
  write_text(word_file, std::string("\x04\x03\x02\x01", 4));
  assert(contains(xdg_magic_database.types_for_file(
                      word_file.string(), word_file.filename().string()),
                  "application/x-host-word"));

  const auto priority_file = root / "priority.unknown";
  write_text(priority_file, "PRIO");
  const auto priority_types = xdg_magic_database.types_for_file(
      priority_file.string(), priority_file.filename().string());
  assert(contains(priority_types, "application/x-priority-high"));
  assert(!contains(priority_types, "application/x-priority-low"));

  const auto codec_file = root / "codec.ogg";
  write_text(codec_file, "CODEC");
  const auto codec_types =
      xdg_magic_database.types_for_file(codec_file.string(), "codec.ogg");
  assert(contains(codec_types, "audio/x-vorbis+ogg"));
  assert(xdg_magic_database.matches(codec_types, "audio/x-vorbis+ogg"));
  assert(xdg_magic_database.matches(codec_types, "audio/ogg"));

  const auto old_magic_file = root / "old-magic.unknown";
  write_text(old_magic_file, "OLD");
  const auto old_magic_types = xdg_magic_database.types_for_file(
      old_magic_file.string(), old_magic_file.filename().string());
  assert(!contains(old_magic_types, "application/x-overridden"));
  const auto new_magic_file = root / "new-magic.unknown";
  write_text(new_magic_file, "NEW");
  const auto new_magic_types = xdg_magic_database.types_for_file(
      new_magic_file.string(), new_magic_file.filename().string());
  assert(contains(new_magic_types, "application/x-overridden"));

  std::filesystem::remove_all(root);
  return 0;
}
