#!/usr/bin/env python3
"""Call the Kit service from scripts (auto-starts it).

    python3 -I tools/kit_service/cli.py <method> [json-params]
    python3 -I tools/kit_service/cli.py backend '{"action":"catalog","disc":"/path/to/disc"}'
    python3 -I tools/kit_service/cli.py index.search '{"disc":"/path/to/disc","role":"sky"}'
    python3 -I tools/kit_service/cli.py job.list

Prints the JSON result. Exit 0 on success, 1 on an application error (printed
as {"error": ...}), 2 when the service cannot be reached or started.
"""
import json
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parents[1]))

from kit_service.client import Client  # noqa: E402
from kit_service.common import KitError, ServiceUnavailable  # noqa: E402


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    method = argv[0]
    params = json.loads(argv[1]) if len(argv) > 1 else {}
    if len(argv) > 1 and argv[1] == "-":
        params = json.load(sys.stdin)
    try:
        result = Client().call(method, params)
    except KitError as error:
        print(json.dumps(dict(error=error.payload()), indent=1))
        return 1
    except ServiceUnavailable as error:
        print(json.dumps(dict(error=dict(code="SERVICE_UNAVAILABLE", message=str(error)))), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
