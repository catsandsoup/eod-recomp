#!/usr/bin/env python3
"""Fail the R7 runtime graphics gate on a nonzero or missing shutdown summary."""

import argparse
import re
from pathlib import Path


SUMMARY = re.compile(
    r"\[r7-audit\] vu1=([0-9]+) gif_qwords=([0-9]+) gs_raster=([0-9]+) gs_suppressed=([0-9]+) "
    r"sync_vram=([0-9]+) deferred_wait=([0-9]+) clut_wait=([0-9]+)"
)
FIELDS = ("vu1", "gif_qwords", "gs_raster", "gs_suppressed", "sync_vram", "deferred_wait", "clut_wait")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="One complete game run per log")
    args = parser.parse_args()
    failed = False
    for path in args.logs:
        try:
            log = path.read_text(errors="replace")
        except OSError as error:
            print(f"{path}: FAIL: {error}")
            failed = True
            continue
        summaries = [line for line in log.splitlines() if line.startswith("[r7-audit]")]
        if len(summaries) != 1:
            print(f"{path}: FAIL: expected one R7 shutdown summary, found {len(summaries)}")
            failed = True
            continue
        match = SUMMARY.fullmatch(summaries[0])
        if match is None:
            print(f"{path}: FAIL: malformed R7 shutdown summary")
            failed = True
            continue
        nonzero = [f"{key}={value}" for key, value in zip(FIELDS, match.groups()) if value.lstrip("0")]
        if nonzero:
            print(f"{path}: FAIL: {' '.join(nonzero)}")
            failed = True
        else:
            print(f"{path}: PASS: zero in all R7 runtime counters")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
