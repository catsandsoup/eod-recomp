"""EOD Creation Kit persistent backend service (docs/KIT_SERVICE.md).

One long-lived process per Kit workspace serves the modding_backend actions,
the SQLite asset index and the persistent job queue over newline-delimited
JSON-RPC 2.0 (Unix domain socket, or stdio). Standard library only; must run on
the macOS system Python (3.9).
"""
VERSION = "1.0.0"
