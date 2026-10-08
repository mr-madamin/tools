#include "json.hpp"

#include <cmath>
#include <cstdlib>
#include <format>

namespace lfs::json {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> parse_document()
    {
        auto value = parse_value();
        if (!value)
            return std::nullopt;
        skip_ws();
        if (at_ < text_.size())
            return std::nullopt; // trailing garbage
        return value;
    }

private:
    std::string_view text_;
    std::size_t at_ = 0;
    int depth_ = 0;

    int peek() const { return at_ < text_.size() ? static_cast<unsigned char>(text_[at_]) : -1; }

    void skip_ws()
    {
        while (at_ < text_.size()) {
            char c = text_[at_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            at_++;
        }
    }

    bool literal(std::string_view word)
    {
        if (text_.compare(at_, word.size(), word) != 0)
            return false;
        at_ += word.size();
        return true;
    }

    // Encode one code point as UTF-8, the way Python's json decoder would.
    static void emit_utf8(std::string& out, unsigned int cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::optional<unsigned int> hex4()
    {
        if (text_.size() - at_ < 4)
            return std::nullopt;
        unsigned int value = 0;
        for (int i = 0; i < 4; i++) {
            char c = text_[at_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned>(c - 'A' + 10);
            else
                return std::nullopt;
        }
        at_ += 4;
        return value;
    }

    std::optional<std::string> parse_string()
    {
        if (peek() != '"')
            return std::nullopt;
        at_++;

        std::string out;
        while (at_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[at_++]);

            if (c == '"')
                return out;

            if (c != '\\') {
                if (c < 0x20)
                    return std::nullopt; // raw control character
                out += static_cast<char>(c);
                continue;
            }

            if (at_ >= text_.size())
                return std::nullopt;
            char esc = text_[at_++];
            switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto cp = hex4();
                if (!cp)
                    return std::nullopt;
                unsigned int value = *cp;
                if (value >= 0xD800 && value <= 0xDBFF) { // high surrogate
                    if (text_.compare(at_, 2, "\\u") == 0) {
                        std::size_t save = at_;
                        at_ += 2;
                        auto low = hex4();
                        if (low && *low >= 0xDC00 && *low <= 0xDFFF)
                            value = 0x10000 + ((value - 0xD800) << 10) + (*low - 0xDC00);
                        else
                            at_ = save; // not a pair; leave the escape alone
                    }
                }
                emit_utf8(out, value);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt; // unterminated
    }

    // JSON's number grammar, checked before strtod sees it. Being strict here
    // costs nothing and rejects what Python rejects ("+1", "01", ".5", "nan"),
    // where C's parser handed the token to strtod and took whatever it liked.
    // "1e999" is deliberately still accepted: strtod returns inf, and the PUT
    // header's range check is what refuses it -- with a message that names the
    // field, rather than a parse error that names nothing.
    std::optional<Value> parse_number()
    {
        std::size_t start = at_;
        if (peek() == '-')
            at_++;

        if (peek() == '0')
            at_++;
        else if (peek() >= '1' && peek() <= '9')
            while (peek() >= '0' && peek() <= '9')
                at_++;
        else
            return std::nullopt;

        if (peek() == '.') {
            at_++;
            if (!(peek() >= '0' && peek() <= '9'))
                return std::nullopt;
            while (peek() >= '0' && peek() <= '9')
                at_++;
        }
        if (peek() == 'e' || peek() == 'E') {
            at_++;
            if (peek() == '+' || peek() == '-')
                at_++;
            if (!(peek() >= '0' && peek() <= '9'))
                return std::nullopt;
            while (peek() >= '0' && peek() <= '9')
                at_++;
        }

        std::string token(text_.substr(start, at_ - start));
        char* tail = nullptr;
        double d = std::strtod(token.c_str(), &tail);
        if (tail != token.c_str() + token.size())
            return std::nullopt;
        return Value(Storage(d));
    }

    std::optional<Value> parse_value()
    {
        skip_ws();

        switch (peek()) {
        case '{':
        case '[':
            // The only two recursive branches; guard them both in one place.
            if (depth_ >= MAX_DEPTH)
                return std::nullopt;
            depth_++;
            break;
        default:
            break;
        }

        std::optional<Value> result;

        switch (peek()) {
        case '{': {
            at_++;
            Value::Object object;
            skip_ws();
            if (peek() == '}') {
                at_++;
                result = Value(Storage(std::move(object)));
                break;
            }
            for (;;) {
                skip_ws();
                auto key = parse_string();
                if (!key)
                    return std::nullopt;
                skip_ws();
                if (peek() != ':')
                    return std::nullopt;
                at_++;
                auto value = parse_value();
                if (!value)
                    return std::nullopt;
                object.insert_or_assign(std::move(*key), std::move(*value));
                skip_ws();
                if (peek() == ',') {
                    at_++;
                    continue;
                }
                if (peek() == '}') {
                    at_++;
                    result = Value(Storage(std::move(object)));
                    break;
                }
                return std::nullopt;
            }
            break;
        }
        case '[': {
            at_++;
            Value::Array array;
            skip_ws();
            if (peek() == ']') {
                at_++;
                result = Value(Storage(std::move(array)));
                break;
            }
            for (;;) {
                auto value = parse_value();
                if (!value)
                    return std::nullopt;
                array.push_back(std::move(*value));
                skip_ws();
                if (peek() == ',') {
                    at_++;
                    continue;
                }
                if (peek() == ']') {
                    at_++;
                    result = Value(Storage(std::move(array)));
                    break;
                }
                return std::nullopt;
            }
            break;
        }
        case '"': {
            auto s = parse_string();
            if (!s)
                return std::nullopt;
            result = Value(Storage(std::move(*s)));
            break;
        }
        case 't':
            if (!literal("true"))
                return std::nullopt;
            result = Value(Storage(true));
            break;
        case 'f':
            if (!literal("false"))
                return std::nullopt;
            result = Value(Storage(false));
            break;
        case 'n':
            if (!literal("null"))
                return std::nullopt;
            result = Value(Storage(std::monostate{}));
            break;
        default:
            result = parse_number();
            break;
        }

        if (result && (result->is_object() || result->as_array()))
            depth_--;
        else if (!result)
            return std::nullopt;
        return result;
    }
};

} // namespace

const Value* Value::find(std::string_view key) const
{
    const auto* object = as_object();
    if (object == nullptr)
        return nullptr;
    auto it = object->find(key);
    return it == object->end() ? nullptr : &it->second;
}

const std::string* Value::str(std::string_view key) const
{
    const Value* v = find(key);
    return v != nullptr ? v->as_string() : nullptr;
}

std::optional<double> Value::number(std::string_view key) const
{
    const Value* v = find(key);
    if (v == nullptr)
        return std::nullopt;
    const double* d = v->as_number();
    return d != nullptr ? std::optional<double>(*d) : std::nullopt;
}

std::optional<Value> parse(std::string_view text)
{
    return Parser(text).parse_document();
}

std::string escape(std::string_view s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else
                out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

std::string py_repr(const Value* v)
{
    if (v == nullptr || v->is_null())
        return "None";
    if (const auto* s = v->as_string())
        return "'" + *s + "'";
    if (const auto* b = v->as_bool())
        return *b ? "True" : "False";
    if (const auto* d = v->as_number()) {
        if (*d == std::floor(*d) && std::isfinite(*d) && std::abs(*d) < 1e15)
            return std::format("{}", static_cast<long long>(*d));
        return std::format("{}", *d);
    }
    if (v->as_array())
        return "[...]";
    return "{...}";
}

} // namespace lfs::json
