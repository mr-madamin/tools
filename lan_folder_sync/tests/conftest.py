"""Make the project modules (framing.py, sync_server.py, ...) importable to the
runners in this folder.

Why this file exists: pytest's default "prepend" import mode puts *this*
directory (tests/) on sys.path -- not its parent -- so `from framing import ...`
would not resolve. We insert the parent (the project root, where framing.py
lives) instead. pytest auto-loads conftest.py before collecting anything, so the
fix is in place by the time a runner is imported.

This only takes effect under pytest. To run a single runner directly (each needs
a live server in another terminal), invoke it as a module from the project root
so the current directory supplies `framing`:

    cd lan_folder_sync
    python3 -m tests.bad_frame_test
"""
import os
import sys

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, PROJECT_ROOT)
