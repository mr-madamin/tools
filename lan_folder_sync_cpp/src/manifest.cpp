#include "manifest.hpp"

#include "util.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <format>
#include <sys/stat.h>

namespace lfs {
namespace {

// opendir/readdir, not std::filesystem::recursive_directory_iterator. The
// iterator's two modes are both wrong here: without an error_code it throws
// halfway through a scan, and with skip_permission_denied it walks past an
// unreadable directory in silence -- the same hole os.walk(onerror=None) had,
// which let --delete remove files the walk could not see. We need the third
// behaviour: keep walking AND remember that the picture has a hole in it.
//
// `prefix` is the path relative to the root; `full` is the directory being read.
bool walk(const std::string& full, std::string& prefix, Manifest& m)
{
    DIR* dir = opendir(full.c_str());
    if (dir == nullptr)
        return false;

    std::size_t prefix_len = prefix.size();
    bool complete = true; // sticky: any unreadable subtree makes the walk partial

    while (struct dirent* entry = readdir(dir)) {
        std::string name(entry->d_name);
        if (name == "." || name == "..")
            continue;

        std::string child = path_join(full, name);

        struct stat lst{};
        if (lstat(child.c_str(), &lst) != 0) {
            // ENOENT is a benign race: something was deleted while we walked,
            // and it genuinely isn't there any more. Anything else (a path past
            // PATH_MAX, a directory we lack permission to enter) means the
            // entry EXISTS and we simply cannot see it -- which is exactly the
            // silent hole that made this listing look complete when it wasn't.
            if (errno != ENOENT)
                complete = false;
            continue;
        }

        prefix.resize(prefix_len);
        if (!prefix.empty())
            prefix += '/';
        prefix += name;

        if (S_ISDIR(lst.st_mode)) {
            if (!walk(child, prefix, m))
                complete = false;
        } else if (S_ISLNK(lst.st_mode)) {
            // A symlinked file still shows up, stat()ed through the link.
            struct stat st{};
            if (stat(child.c_str(), &st) != 0) {
                // A broken link: not a file we can send, and not a directory we
                // failed to read. Not an error -- but say so rather than let
                // the user wonder.
                m.skipped_links.push_back(prefix);
            } else if (S_ISDIR(st.st_mode)) {
                m.skipped_links.push_back(prefix); // a real directory we choose not to follow
            } else {
                m.files.insert_or_assign(prefix, Entry{static_cast<long long>(st.st_size),
                                                       stat_mtime(st)});
            }
        } else {
            m.files.insert_or_assign(prefix, Entry{static_cast<long long>(lst.st_size),
                                                   stat_mtime(lst)});
        }
    }

    prefix.resize(prefix_len);
    closedir(dir);
    return complete;
}

} // namespace

Manifest Manifest::scan(const std::string& root_dir)
{
    Manifest m;
    std::string prefix;
    m.complete = walk(root_dir, prefix, m);
    return m;
}

std::string Manifest::to_json() const
{
    std::string out = "{";
    bool first = true;
    for (const auto& [path, entry] : files) {
        if (!first)
            out += ", ";
        first = false;
        out += std::format("{}: {{\"size\": {}, \"mtime\": {:.6f}}}", json::escape(path),
                           entry.size, entry.mtime);
    }
    out += '}';
    return out;
}

std::optional<Manifest> Manifest::from_json(const json::Value* files)
{
    if (files == nullptr)
        return std::nullopt;
    const auto* object = files->as_object();
    if (object == nullptr)
        return std::nullopt;

    Manifest m;
    for (const auto& [path, info] : *object) {
        auto size = info.number("size");
        auto mtime = info.number("mtime");
        if (!size || !mtime)
            continue;
        m.files.insert_or_assign(path, Entry{static_cast<long long>(*size), *mtime});
    }
    return m;
}

Plan diff_manifests(const Manifest& local, const Manifest& remote, double tolerance)
{
    Plan plan;

    for (const auto& [path, mine] : local.files) {
        auto it = remote.files.find(path);
        if (it == remote.files.end()) {
            plan.to_put.push_back(path); // the peer doesn't have it
            continue;
        }
        if (mine.size != it->second.size || std::fabs(mine.mtime - it->second.mtime) > tolerance)
            plan.to_put.push_back(path);
    }

    for (const auto& [path, _] : remote.files) {
        if (!local.files.contains(path))
            plan.to_delete.push_back(path); // we don't have it
    }

    return plan;
}

} // namespace lfs
