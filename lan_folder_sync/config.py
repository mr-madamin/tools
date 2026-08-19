import json
import os
import sys

CONFIG_PATH = os.path.join(os.path.dirname(__file__), "config.json")


def load_config():
    try:
        with open(CONFIG_PATH) as f:
            return json.load(f)
    except FileNotFoundError:
        sys.exit(
            "config.json not found. Copy config.example.json to config.json and set "
            "shared_dir, peer host/port, and a shared token (same on both machines)."
        )
