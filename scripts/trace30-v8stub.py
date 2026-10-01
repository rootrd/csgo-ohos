#!/usr/bin/env python3
"""Retired: a symbol-only V8 library cannot run Panorama."""
import sys

sys.exit(
    "trace30-v8stub.py is retired: skipping V8 scopes does not disable "
    "Panorama safely and also invalidates the scope lifetime with real V8. "
    "Use scripts/build-ohos-v8.sh and the Panorama runtime smoke test; "
    "see docs/PANORAMA-RUNTIME.md. No source files were changed."
)
