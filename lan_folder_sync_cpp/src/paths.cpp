#include "paths.hpp"

#include "util.hpp"

#include <climits>
#include <cstdlib>
#include <sys/stat.h>

namespace lfs {

bool path_is_lexically_safe(std::string_view rel_path)
{
    if (rel_path.empty() || rel_path.front() == '/')
        return false;

    std::size_t at = 0;
    while (at <= rel_path.size()) {
        auto slash = rel_path.find('/', at);
        auto component = rel_path.substr(at, slash == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : slash - at);
        if (component == "..")
            return false;
        if (slash == std::string_view::npos)
            break;
        at = slash + 1;
    }
    return true;
}

std::optional<std::string> safe_path(const std::string& base, std::string_view rel_path)
{
    char base_real[PATH_MAX];
    if (realpath(base.c_str(), base_real) == nullptr)
        return std::nullopt;
    if (!path_is_lexically_safe(rel_path))
        return std::nullopt;

    std::string root(base_real);
    std::string current = root;

    std::size_t at = 0;
    while (at < rel_path.size()) {
        auto slash = rel_path.find('/', at);
        auto component = rel_path.substr(at, slash == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : slash - at);
        at = slash == std::string_view::npos ? rel_path.size() : slash + 1;

        if (component.empty() || component == ".")
            continue;

        std::string next = path_join(current, component);

        struct stat lst{};
        if (lstat(next.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode)) {
            char resolved[PATH_MAX];
            if (realpath(next.c_str(), resolved) == nullptr)
                return std::nullopt; // broken link -- refuse it
            next = resolved;
        }

        // Byte comparison, deliberately: this is the check that a Unicode-aware
        // one would get wrong (Swift's String == calls "e\u{301}" equal to "é",
        // the filesystem does not). std::string gives us bytes by default.
        if (next.compare(0, root.size(), root) != 0)
            return std::nullopt;
        if (next.size() > root.size() && next[root.size()] != '/')
            return std::nullopt;

        current = std::move(next);
    }

    if (current == root)
        return std::nullopt; // the path named the root itself
    return current;
}

} // namespace lfs
