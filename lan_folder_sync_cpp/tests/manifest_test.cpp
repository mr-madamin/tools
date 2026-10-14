// Walking a folder, saying what the walk could NOT see, and the diff window.
#include "manifest.hpp"
#include "support.hpp"
#include "test.hpp"

#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

using namespace support;

namespace {

bool lists(const std::vector<std::string>& items, std::string_view wanted)
{
    return std::ranges::find(items, wanted) != items.end();
}

} // namespace

TEST(Manifest, lists_files_with_their_size_and_mtime)
{
    TempDir dir;
    write_file(dir.path() + "/a.txt", "hello");
    write_file(dir.path() + "/sub/b.txt", "world!");
    set_mtime(dir.path() + "/a.txt", 1700000000.5);

    Manifest m = Manifest::scan(dir.path());
    CHECK(m.complete, "a readable tree was reported as incomplete");
    CHECK(m.files.size() == 2, "found {} files, expected 2", m.files.size());
    CHECK(m.files.at("a.txt").size == 5, "wrong size for a.txt");
    CHECK(m.files.at("sub/b.txt").size == 6, "wrong size for sub/b.txt");
    CHECK(std::abs(m.files.at("a.txt").mtime - 1700000000.5) < 1e-6, "wrong mtime for a.txt");
}

TEST(Manifest, survives_the_json_round_trip_with_its_mtimes)
{
    TempDir dir;
    write_file(dir.path() + "/a.txt", "hello");
    set_mtime(dir.path() + "/a.txt", 1700000000.123456);

    Manifest m = Manifest::scan(dir.path());
    auto parsed = json::parse(m.to_json());
    CHECK(parsed.has_value(), "the manifest we generated is not valid JSON: {}", m.to_json());

    auto back = Manifest::from_json(&*parsed);
    CHECK(back.has_value(), "a manifest we generated did not parse back");
    CHECK(back->files.size() == 1, "round trip lost files");
    // The diff's window is 1 ms, so the wire form has to keep more precision
    // than that or every file would look changed on every run.
    CHECK(std::abs(back->files.at("a.txt").mtime - 1700000000.123456) < 1e-6,
          "mtime lost precision on the wire: {:.6f}", back->files.at("a.txt").mtime);
}

TEST(Manifest, a_symlinked_file_is_followed_and_a_symlinked_directory_is_not)
{
    TempDir dir;
    TempDir outside;
    write_file(outside.path() + "/target.txt", "linked content");
    write_file(outside.path() + "/nested/deep.txt", "should not be walked");

    CHECK(::symlink((outside.path() + "/target.txt").c_str(),
                    (dir.path() + "/link_to_file.txt").c_str()) == 0,
          "symlink to file failed");
    CHECK(::symlink((outside.path() + "/nested").c_str(), (dir.path() + "/link_to_dir").c_str()) == 0,
          "symlink to dir failed");
    CHECK(::symlink("/nowhere/at/all", (dir.path() + "/broken").c_str()) == 0,
          "broken symlink failed");

    Manifest m = Manifest::scan(dir.path());

    CHECK(m.complete, "symlinks made the walk look incomplete");
    CHECK(m.files.contains("link_to_file.txt"), "a symlink to a file was not synced");
    CHECK(m.files.at("link_to_file.txt").size == 14, "the link was stat'd, not its target");
    CHECK(!m.files.contains("link_to_dir/deep.txt"), "a symlinked directory was followed");
    CHECK(lists(m.skipped_links, "link_to_dir"), "the skipped directory link was not reported");
    CHECK(lists(m.skipped_links, "broken"), "the broken link was not reported");
}

TEST(Manifest, an_unreadable_directory_makes_the_walk_partial)
{
    if (::getuid() == 0)
        return; // chmod 000 would not block root
    TempDir dir;
    write_file(dir.path() + "/visible.txt", "here");
    write_file(dir.path() + "/hidden/secret.txt", "still here");
    CHECK(::chmod((dir.path() + "/hidden").c_str(), 0000) == 0, "chmod failed");

    Manifest m = Manifest::scan(dir.path());
    ::chmod((dir.path() + "/hidden").c_str(), 0755); // or the temp dir can't be removed

    // The file is STILL THERE; we just can't see it. To a diff that looks
    // exactly like a file you deleted, which is why the pusher refuses
    // --delete on this.
    CHECK(!m.complete, "an unreadable directory was not reported");
    CHECK(!m.files.contains("hidden/secret.txt"), "the unreadable file was somehow listed");
    CHECK(m.files.contains("visible.txt"), "the rest of the walk was abandoned");
}

TEST(diff, sends_what_differs_and_nothing_else)
{
    Manifest local, remote;
    local.files = {
        {"same.txt", {100, 1700000000.0}},
        {"changed_size.txt", {100, 1700000000.0}},
        {"changed_time.txt", {100, 1700000000.0}},
        {"only_local.txt", {1, 1700000000.0}},
        {"within_window.txt", {100, 1700000000.0}},
    };
    remote.files = {
        {"same.txt", {100, 1700000000.0}},
        {"changed_size.txt", {101, 1700000000.0}},
        {"changed_time.txt", {100, 1700000002.0}},
        {"only_remote.txt", {1, 1700000000.0}},
        // Inside the 1 ms window: round-trip error, not an edit.
        {"within_window.txt", {100, 1700000000.0004}},
    };

    Plan plan = diff_manifests(local, remote);

    CHECK(lists(plan.to_put, "changed_size.txt"), "a size change was not sent");
    CHECK(lists(plan.to_put, "changed_time.txt"), "an mtime change was not sent");
    CHECK(lists(plan.to_put, "only_local.txt"), "a new file was not sent");
    CHECK(!lists(plan.to_put, "same.txt"), "an unchanged file was resent");
    CHECK(!lists(plan.to_put, "within_window.txt"), "a sub-millisecond difference was resent");
    CHECK(plan.to_delete == std::vector<std::string>{"only_remote.txt"},
          "the delete list was wrong");
}

TEST(diff, catches_an_edit_that_keeps_the_size_and_moves_the_mtime_slightly)
{
    // The window used to be 2 SECONDS, and this is what it swallowed: same
    // size, mtime a second later, unchanged forever because the two never
    // drift further apart.
    Manifest local, remote;
    local.files = {{"notes.txt", {4096, 1700000001.0}}};
    remote.files = {{"notes.txt", {4096, 1700000000.0}}};

    Plan plan = diff_manifests(local, remote);
    CHECK(lists(plan.to_put, "notes.txt"), "a one-second edit was called unchanged");
}
