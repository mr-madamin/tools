// A minimal JSON reader -- the one thing Python handed over as a single
// `import json`. Enough of the spec to speak this protocol: objects, arrays,
// strings (with \u escapes and surrogate pairs), numbers, true/false/null.
//
// Output is built by hand instead (see Framing/Manifest), because the wire
// format pins the spelling of a few values: `size` is an integer, `mtime` is
// "%.6f", and a generic dumper would render both however it liked.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lfs::json {

class Value;

// std::variant is doing real work here: a bool is a *different alternative*
// from a number, so `{"size": true}` misses `as_number()` without a special
// case. Python needed one (bool is a subclass of int, so isinstance(True, int)
// is True and `true` compared equal to 1), and C's tagged struct needed the
// type check written out by hand at every call site.
using Storage = std::variant<std::monostate, // null
                             bool,
                             double,
                             std::string,
                             std::vector<Value>,                        // array
                             std::map<std::string, Value, std::less<>>>; // object

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    explicit Value(Storage storage) : storage_(std::move(storage)) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(storage_); }
    bool is_object() const { return std::holds_alternative<Object>(storage_); }

    const std::string* as_string() const { return std::get_if<std::string>(&storage_); }
    const double* as_number() const { return std::get_if<double>(&storage_); }
    const bool* as_bool() const { return std::get_if<bool>(&storage_); }
    const Array* as_array() const { return std::get_if<Array>(&storage_); }
    const Object* as_object() const { return std::get_if<Object>(&storage_); }

    // nullptr when this isn't an object, or the key is absent.
    const Value* find(std::string_view key) const;

    // Present-and-a-string, or nullptr. A caller that needs to tell "absent"
    // from "wrong type" asks find() as well -- the PUT header refusals do,
    // because "missing field" and "out-of-range" are different bugs.
    const std::string* str(std::string_view key) const;
    std::optional<double> number(std::string_view key) const;

private:
    Storage storage_;
};

// std::nullopt when the text isn't valid JSON, has trailing garbage, or nests
// deeper than the depth cap.
std::optional<Value> parse(std::string_view text);

// Nesting costs a stack frame per '[' in a recursive-descent parser, and the
// header is parsed BEFORE the HELLO: 200 KB of '[' walked the C build off the
// end of its stack and killed the server. Real payloads nest three deep.
constexpr int MAX_DEPTH = 64;

// `s` as a quoted, escaped JSON string.
std::string escape(std::string_view s);

// How Python's %r would print this value, so the two servers' `unknown op:`
// lines read the same: 'MANFEST', None, 5, True.
std::string py_repr(const Value* v);

} // namespace lfs::json
