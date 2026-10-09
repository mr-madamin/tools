// Small helpers the standard library doesn't hand over: byte-level path
// plumbing (std::filesystem would give us Unicode-aware comparison and
// lexical ".." folding, neither of which is what the protocol means), UTF-8
// validation, and the two argv guards.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace lfs {

// Print to stderr and exit 1. Call sites format with std::format, so there is
// one overload and no chance of a format string coming from the wire.
[[noreturn]] void die(std::string_view message);

// ---- argv -----------------------------------------------------------------

// Both programs take the same shape of command line: up to two positional
// arguments and a fixed set of --flags. Anything else is a typo, and a typo
// that gets ignored is how --dryrun becomes a real push.
struct Args {
    std::vector<std::string> positional; // in order, capped by the caller
    std::vector<std::string> flags;      // the recognised ones that were given
    std::vector<std::string> unknown;    // everything else starting with --

    bool has(std::string_view flag) const;
    const std::string* at(std::size_t index) const; // nullptr when not given
};

Args parse_args(int argc, char** argv, const std::vector<std::string>& known,
                std::size_t max_positional = 2);

// Exit listing the unknown --flags (sorted, deduped) above a usage line. A
// silently-ignored typo like --dryrun is how a preview becomes a real push.
[[noreturn]] void die_unknown_flags(std::vector<std::string> unknown,
                                    std::string_view usage);

// A TCP port, or die explaining why not. std::stoi would throw on "http" and
// accept "9999junk"; neither is a diagnosis, so parse by hand.
int parse_port(std::string_view text, std::string_view where);

// ---- paths ----------------------------------------------------------------
//
// std::string, not std::filesystem::path: every one of these is compared
// against bytes that arrived over the wire, and fs::path brings along
// lexically_normal() (which folds ".." before we get to refuse it) and
// operator/ (which throws away the left side when the right is absolute --
// exactly the traversal we're guarding). Keeping them as bytes keeps the
// guards honest and matches Python's os.path.

std::string path_join(std::string_view a, std::string_view b); // os.path.join
std::string path_dirname(std::string_view path);               // os.path.dirname
std::string path_abs(std::string_view path);                   // lexical, like abspath
bool mkdir_p(const std::string& path);                         // makedirs(exist_ok=True)

std::optional<std::string> read_whole_file(const std::string& path);
bool write_whole_file(const std::string& path, std::string_view text);

// Python decodes every header with .decode("utf-8") and refuses what doesn't
// fit; we check by hand to refuse the same frames.
bool valid_utf8(std::string_view s);

// st_mtime with sub-second precision, as the JSON float the protocol carries.
double stat_mtime(const struct stat& st);

} // namespace lfs
