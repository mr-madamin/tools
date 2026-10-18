// Package config reads config.json: the token, the folder, and where the peer
// is. It is hand-edited by every user, so a mistyped value names itself rather
// than falling through to a default -- quoting the port by mistake
// ("port": "9999") used to give you 8765 and then a "connection refused" with
// nothing pointing at the config.
package config

import (
	"errors"
	"fmt"
	"os"

	"lanfoldersync/internal/cli"
	"lanfoldersync/internal/jsonval"
)

type Config struct {
	SharedDir string // "" when absent: callers apply their own default
	PeerHost  string
	PeerPort  int // 0 when absent
	Token     string
}

// Error is a config.json we could read but not use. Separating this from the
// exit is what lets the tests assert on these messages without spawning a
// process.
type Error struct {
	Message string
}

func (e *Error) Error() string { return e.Message }

func errorf(format string, args ...any) *Error {
	return &Error{Message: fmt.Sprintf(format, args...)}
}

// configString: a key that is absent is fine -- the defaults are the point. A
// key that is present but the wrong type is a typo, and gets named.
func configString(object jsonval.Object, key, path, where string) (string, error) {
	raw, present := object.Find(key)
	if !present || raw == nil {
		return "", nil // genuinely absent -- the caller decides if that's ok
	}
	value, ok := object.Str(key)
	if !ok {
		return "", errorf("%s: %q must be a string, in double quotes", path, where)
	}
	return value, nil
}

// Parse is the whole of the typing policy, separate from any file I/O.
//
// Note what is NOT used here: unmarshalling into a struct. encoding/json would
// silently accept "port": "9999" only to fail on the type, reporting
// `json: cannot unmarshal string into Go struct field ... of type int` -- a
// message about Go's types, naming a Go field, for a mistake the user made in
// their editor. Worse, it ignores unknown fields and zeroes mistyped ones
// unless you ask otherwise, which is the exact silent-default behaviour this
// package exists to prevent. The error text has to name the user's key.
func Parse(text []byte, path string) (Config, error) {
	var cfg Config

	value, err := jsonval.Parse(text)
	if err != nil {
		return cfg, errorf("%s is not valid JSON", path)
	}
	root, ok := jsonval.AsObject(value)
	if !ok {
		return cfg, errorf("%s is not valid JSON", path)
	}

	if cfg.SharedDir, err = configString(root, "shared_dir", path, "shared_dir"); err != nil {
		return cfg, err
	}

	token, err := configString(root, "token", path, "token")
	if err != nil {
		return cfg, err
	}
	if token == "" {
		return cfg, errorf("%s: missing %q", path, "token")
	}
	cfg.Token = token

	rawPeer, present := root.Find("peer")
	if present && rawPeer != nil {
		peer, ok := jsonval.AsObject(rawPeer)
		if !ok {
			return cfg, errorf(
				"%s: %q must be an object, like {\"host\": \"192.168.1.42\", \"port\": 8765}",
				path, "peer")
		}

		if cfg.PeerHost, err = configString(peer, "host", path, "peer.host"); err != nil {
			return cfg, err
		}

		if rawPort, present := peer.Find("port"); present && rawPort != nil {
			port, isNumber := peer.Number("port")
			if !isNumber {
				return cfg, errorf("%s: peer.port must be a number, not in quotes", path)
			}
			// Same range check as the command line. A JSON number also has to
			// survive the conversion to int, so inf/NaN is rejected before it.
			if !jsonval.Finite(port) || port < 1 || port > 65535 {
				return cfg, errorf("%s: peer.port must be 1-65535", path)
			}
			cfg.PeerPort = int(port)
		}
	}

	return cfg, nil
}

// Find locates config.json. The binaries live in bin/, so the search is a
// little wider than Python's "next to the source file":
// $LAN_FOLDER_SYNC_CONFIG, then ./config.json, then next to the executable,
// then its parent. Same order as the C and C++ ports, so the interop targets
// can point all of them at one file.
func Find(argv0 string) (string, bool) {
	if env := os.Getenv("LAN_FOLDER_SYNC_CONFIG"); env != "" {
		return env, true
	}
	if readable("config.json") {
		return "config.json", true
	}
	if argv0 != "" {
		dir := cli.PathDirname(argv0)
		if here := cli.PathJoin(dir, "config.json"); readable(here) {
			return here, true
		}
		if up := cli.PathJoin(dir, "../config.json"); readable(up) {
			return up, true
		}
	}
	return "", false
}

func readable(path string) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	file.Close()
	return true
}

// Load reads config.json and exits loudly if it isn't there or can't be used.
func Load(argv0 string) Config {
	path, found := Find(argv0)
	var text []byte
	if found {
		var err error
		if text, err = os.ReadFile(path); err != nil {
			found = false
		}
	}

	if !found {
		cli.Die("config.json not found. Copy config.example.json to config.json and set " +
			"shared_dir, peer host/port, and a shared token (same on both machines).")
	}

	cfg, err := Parse(text, path)
	if err != nil {
		var configErr *Error
		if errors.As(err, &configErr) {
			cli.Die(configErr.Message)
		}
		cli.Die(err.Error())
	}
	return cfg
}
