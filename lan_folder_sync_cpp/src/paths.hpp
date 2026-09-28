// The two-stage confinement guard every incoming path goes through, for PUT
// and for DELETE alike.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lfs {

// A cheap first pass: no absolute paths, no ".." component. Runs before we
// create any directories, so a traversal never gets to mkdir anything.
bool path_is_lexically_safe(std::string_view rel_path);

// Resolve `rel_path` under `base` and refuse anything that lands outside it.
// ".." and absolute paths are gone by the time we get here, so the one
// remaining escape is a symlink: walk the path a component at a time and, each
// time a component *is* a link, resolve it and re-check containment.
//
// std::filesystem::weakly_canonical() looks like this function and isn't: it
// folds ".." lexically before resolving, so "link/../x" answers a question
// about a directory the kernel would never visit. fs::canonical() refuses a
// path that doesn't exist yet, which is the normal case for a PUT.
std::optional<std::string> safe_path(const std::string& base, std::string_view rel_path);

} // namespace lfs
