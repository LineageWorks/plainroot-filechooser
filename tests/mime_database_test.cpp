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
  assert(contains(ambiguous_types, "text/plain"));
  assert(!contains(ambiguous_types, "application/x-ambiguous-one"));
  MimeDatabase sniffing_database({low.string(), high.string()}, true);
  if (sniffing_database.content_sniffing_available()) {
    const auto sniffed_ambiguous_types = sniffing_database.types_for_file(
        ambiguous_file.string(), "sample.amb");
    assert(contains(sniffed_ambiguous_types, "text/plain"));
    assert(!contains(sniffed_ambiguous_types, "application/x-ambiguous-one"));
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

  std::filesystem::remove_all(root);
  return 0;
}
