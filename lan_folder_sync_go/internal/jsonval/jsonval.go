// Package jsonval is the thin layer encoding/json needs before it can be
// trusted with frames that arrive from an unauthenticated peer.
//
// Reading is the standard library's job -- its scanner already enforces the
// JSON number grammar, so "+1", "01", ".5" and a bare "nan" are refused
// exactly as Python refuses them, which the C port got wrong by handing the
// token to strtod. Three things still have to be added on top:
//
//  1. A UTF-8 check. json.Unmarshal silently replaces invalid bytes inside a
//     string with U+FFFD instead of failing, where Python's .decode("utf-8")
//     raises. Without this, a frame the other ports refuse would be accepted
//     here with a mangled path.
//  2. A depth cap. encoding/json has its own limit, but it is 10000 -- far
//     past anything this protocol needs, and the header is parsed BEFORE the
//     HELLO. 200 KB of '[' is a four-byte frame that nobody has authenticated.
//  3. UseNumber. Decoding into `any` turns every number into a float64 by
//     default, which throws away the distinction the PUT guard needs: a value
//     that is absent, a value that is the wrong type, and a value that is a
//     number but out of range are three different bugs with three different
//     messages.
//
// Writing is done by hand, because the wire format pins the spelling of two
// values: `size` is an integer and `mtime` is "%.6f". json.Marshal would
// render a float64 in Go's shortest round-trip form (1.7606e+09), which no
// other port emits, and it escapes <, > and & as < by default -- fine for
// a parser, but it makes two implementations' output stop being comparable
// byte for byte.
package jsonval

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

// MaxDepth bounds nesting. Real payloads in this protocol nest three deep.
const MaxDepth = 64

// Object is a decoded JSON object. Values are `any`, holding exactly what
// encoding/json produces with UseNumber: json.Number, string, bool, nil,
// []any or map[string]any. A bool is therefore a *different type* from a
// number, so {"size": true} misses Number() without a special case -- Python
// needed one, because its bool is a subclass of int and `true` compared equal
// to 1.
type Object map[string]any

var (
	// ErrSyntax covers "didn't parse", "trailing garbage" and "not UTF-8":
	// callers that need to tell the user apart from the parser use the
	// HeaderError kinds in DecodeHeader instead.
	ErrSyntax = errors.New("invalid JSON")
	ErrDepth  = errors.New("nested too deeply")
)

// Parse decodes one complete JSON document. Trailing content is an error: a
// frame is one value, and "{} garbage" meaning "{}" would let two peers
// disagree about what they just exchanged.
func Parse(payload []byte) (any, error) {
	if !utf8.Valid(payload) {
		return nil, ErrSyntax
	}
	if depthOf(payload) > MaxDepth {
		return nil, ErrDepth
	}

	dec := json.NewDecoder(bytes.NewReader(payload))
	dec.UseNumber()

	var value any
	if err := dec.Decode(&value); err != nil {
		return nil, ErrSyntax
	}
	// Decode stops at the end of the first value; anything after it (other
	// than whitespace) means this was not a single document.
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		return nil, ErrSyntax
	}
	return value, nil
}

// depthOf is the maximum nesting of brackets outside string literals. It runs
// before the parser so that deep nesting is refused on a cheap byte scan
// rather than by whatever the parser does when it gets there.
func depthOf(payload []byte) int {
	depth, max := 0, 0
	inString, escaped := false, false

	for _, c := range payload {
		if inString {
			switch {
			case escaped:
				escaped = false
			case c == '\\':
				escaped = true
			case c == '"':
				inString = false
			}
			continue
		}
		switch c {
		case '"':
			inString = true
		case '{', '[':
			depth++
			if depth > max {
				max = depth
			}
			// No point counting past the cap -- and this keeps the scan from
			// being a way to make us do work on a 64 MB frame.
			if max > MaxDepth {
				return max
			}
		case '}', ']':
			depth--
		}
	}
	return max
}

// AsObject is the "this parsed, but is it a header?" check. "123" and "[1,2]"
// are valid JSON and not objects.
func AsObject(value any) (Object, bool) {
	object, ok := value.(map[string]any)
	return Object(object), ok
}

// Find reports the raw value and whether the key was present at all. A caller
// that has to tell "absent" from "wrong type" asks this as well as Str/Number
// -- the PUT header refusals do, because they are different messages.
func (o Object) Find(key string) (any, bool) {
	value, ok := o[key]
	return value, ok
}

// Str returns the value when it is present AND a string.
func (o Object) Str(key string) (string, bool) {
	s, ok := o[key].(string)
	return s, ok
}

// Number returns the value when it is present AND a JSON number. The value may
// be infinite: "1e999" is a number that parses, and the caller's range check is
// what names the field. (Returning an error here instead would report a parse
// problem for a frame whose actual problem is the value.)
func (o Object) Number(key string) (float64, bool) {
	raw, ok := o[key].(json.Number)
	if !ok {
		return 0, false
	}
	// ParseFloat hands back ±Inf together with ErrRange, which is exactly what
	// we want to pass along; any other error means the scanner let through
	// something it shouldn't have.
	value, err := strconv.ParseFloat(string(raw), 64)
	if err != nil && !errors.Is(err, strconv.ErrRange) {
		return 0, false
	}
	return value, true
}

// Finite is the check every number off the wire goes through before it is used
// for a length, an offset or a timestamp. In C and C++ an out-of-range
// conversion here is undefined behaviour; Go defines it, but the defined
// answer is a garbage number (an infinite float64 converts to an
// implementation-defined value, not an error), so the check matters just as
// much -- it is simply a wrong file instead of a crash.
func Finite(value float64) bool {
	return !math.IsInf(value, 0) && !math.IsNaN(value)
}

// Escape renders s as a quoted JSON string, matching the other ports byte for
// byte: raw UTF-8 passes through, and only what the grammar requires is
// escaped.
func Escape(s string) string {
	var out strings.Builder
	out.Grow(len(s) + 2)
	out.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '"':
			out.WriteString(`\"`)
		case '\\':
			out.WriteString(`\\`)
		case '\b':
			out.WriteString(`\b`)
		case '\f':
			out.WriteString(`\f`)
		case '\n':
			out.WriteString(`\n`)
		case '\r':
			out.WriteString(`\r`)
		case '\t':
			out.WriteString(`\t`)
		default:
			if c < 0x20 {
				fmt.Fprintf(&out, `\u%04x`, c)
			} else {
				out.WriteByte(c)
			}
		}
	}
	out.WriteByte('"')
	return out.String()
}

// PyRepr prints a value the way Python's %r would, so every port's
// `unknown op:` and `unsafe path refused:` lines read the same: 'MANFEST',
// None, 5, True.
func PyRepr(value any, present bool) string {
	if !present || value == nil {
		return "None"
	}
	switch v := value.(type) {
	case string:
		return "'" + v + "'"
	case bool:
		if v {
			return "True"
		}
		return "False"
	case json.Number:
		if f, err := strconv.ParseFloat(string(v), 64); err == nil {
			if f == math.Trunc(f) && Finite(f) && math.Abs(f) < 1e15 {
				return strconv.FormatInt(int64(f), 10)
			}
			return strconv.FormatFloat(f, 'g', -1, 64)
		}
		return string(v)
	case []any:
		return "[...]"
	default:
		return "{...}"
	}
}
