#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <unistd.h>

namespace lfs {

void die(std::string_view message)
{
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
    std::exit(1);
}

bool Args::has(std::string_view flag) const
{
    return std::ranges::find(flags, flag) != flags.end();
}

const std::string* Args::at(std::size_t index) const
{
    return index < positional.size() ? &positional[index] : nullptr;
}

Args parse_args(int argc, char** argv, const std::vector<std::string>& known,
                std::size_t max_positional)
{
    Args args;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg.starts_with("--")) {
            if (std::ranges::find(known, arg) != known.end())
                args.flags.push_back(arg);
            else
                args.unknown.push_back(arg);
        } else if (args.positional.size() < max_positional) {
            args.positional.push_back(arg);
        }
    }
    return args;
}

void die_unknown_flags(std::vector<std::string> unknown, std::string_view usage)
{
    std::ranges::sort(unknown);
    unknown.erase(std::ranges::unique(unknown).begin(), unknown.end());

    std::string message = "unknown flag(s):";
    for (const auto& flag : unknown)
        message += " " + flag;
    die(std::format("{}\n{}", message, usage));
}

int parse_port(std::string_view text, std::string_view where)
{
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }))
        die(std::format("{}: '{}' is not a number", where, text));

    // Accumulate with a ceiling rather than converting first: a long string of
    // digits must not overflow on its way to being rejected.
    long value = 0;
    for (char c : text) {
        value = value * 10 + (c - '0');
        if (value > 65535)
            die(std::format("{}: port {} is outside 1-65535", where, text));
    }
    if (value < 1)
        die(std::format("{}: port {} is outside 1-65535", where, text));
    return static_cast<int>(value);
}

std::string path_join(std::string_view a, std::string_view b)
{
    if (!b.empty() && b.front() == '/')
        return std::string(b); // absolute right side wins, as os.path.join does
    if (a.empty())
        return std::string(b);

    std::string joined(a);
    if (joined.back() != '/')
        joined += '/';
    joined += b;
    return joined;
}

std::string path_dirname(std::string_view path)
{
    auto slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return ".";
    if (slash == 0)
        return "/";
    return std::string(path.substr(0, slash));
}

// Lexical only, like os.path.abspath: no realpath(), so it still names a path
// that doesn't exist -- which is exactly the case we want to print about.
std::string path_abs(std::string_view path)
{
    if (!path.empty() && path.front() == '/')
        return std::string(path);

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr)
        return std::string(path);
    return path_join(cwd, path);
}

bool mkdir_p(const std::string& path)
{
    std::string work;
    work.reserve(path.size());

    for (std::size_t i = 0; i < path.size(); i++) {
        work += path[i];
        bool last = (i + 1 == path.size());
        if (path[i] != '/' && !last)
            continue;
        if (work == "/" || work == "." || work == "./")
            continue;

        std::string component = work;
        if (component.size() > 1 && component.back() == '/')
            component.pop_back();
        if (::mkdir(component.c_str(), 0777) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}

std::optional<std::string> read_whole_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_whole_file(const std::string& path, std::string_view text)
{
    if (!mkdir_p(path_dirname(path)))
        return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

bool valid_utf8(std::string_view s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t len = s.size();
    std::size_t i = 0;

    while (i < len) {
        unsigned char c = p[i];
        std::size_t extra;
        unsigned int cp;

        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }

        if (i + extra >= len)
            return false;
        for (std::size_t k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if (extra == 1 && cp < 0x80)
            return false; // overlong
        if (extra == 2 && cp < 0x800)
            return false;
        if (extra == 3 && cp < 0x10000)
            return false;
        if (cp > 0x10FFFF)
            return false;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return false; // lone surrogate
        i += extra + 1;
    }
    return true;
}

double stat_mtime(const struct stat& st)
{
#if defined(__APPLE__)
    return static_cast<double>(st.st_mtimespec.tv_sec) + st.st_mtimespec.tv_nsec / 1e9;
#else
    return static_cast<double>(st.st_mtim.tv_sec) + st.st_mtim.tv_nsec / 1e9;
#endif
}

} // namespace lfs
