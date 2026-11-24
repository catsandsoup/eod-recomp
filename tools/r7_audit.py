#!/usr/bin/env python3
"""Fail the R7 runtime gate on a nonzero or missing shutdown summary."""

import argparse
from collections import Counter
import re
from pathlib import Path


PREFIX = "[r7-audit] v=2 "
FIELDS = ("vu1", "gif_qwords", "gs_raster", "gs_suppressed", "sync_vram", "deferred_wait", "clut_wait",
          "vu0_mlist", "vu0_micro", "vu0_macro", "vu0_control")
SUMMARY = re.compile(re.escape(PREFIX) + " ".join(f"{field}=([0-9]+)" for field in FIELDS))

CENSUS_ROW = re.compile(
    r"\[r7-vu\] vu=([01]) kind=([a-z0-9_]+) address=([0-9a-f]{8}) image=([0-9a-f]{16}) "
    r"context=([0-9a-f]{8}) count=([1-9][0-9]*)"
)
CENSUS_END = re.compile(r"\[r7-vu\] vu=([01]) rows=([0-9]+) dropped=([0-9]+)")
MICRO_SOURCES = {"direct", "mscal", "mscalf", "mscnt", "vcallms", "vcallmsr", "cmsar1"}
FIXED_RETIREMENTS = {"retired_0000": 0, "retired_2538": 0x2538, "retired_25f0": 0x25F0,
                     "retired_queue_dent": 0x0D48}
CENSUS_FIELDS = ("vu1", "vu0_micro", "vu0_macro", "vu0_control")


def check_vu_census(lines, audit):
    rows = set()
    unit_rows = Counter()
    counts = Counter()
    ended = set()
    for line in lines:
        if not line.startswith("[r7-vu]"):
            continue
        end = CENSUS_END.fullmatch(line)
        if end:
            unit, num_rows, dropped = end.groups()
            if unit in ended or int(dropped) or int(num_rows) != unit_rows[unit]:
                raise ValueError(f"incomplete or truncated VU census for VU{unit}")
            ended.add(unit)
            continue
        row = CENSUS_ROW.fullmatch(line)
        if not row:
            raise ValueError("malformed VU census row")
        unit, kind, address, image, context, count = row.groups()
        key = (unit, kind, address, image, context)
        if unit in ended or key in rows:
            raise ValueError("duplicate or late VU census row")
        rows.add(key)
        unit_rows[unit] += 1
        if kind in MICRO_SOURCES:
            if (unit == "1" and kind in {"vcallms", "vcallmsr"}) or (unit == "0" and kind == "cmsar1"):
                raise ValueError(f"invalid launch source {kind} for VU{unit}")
            field = "vu1" if unit == "1" else "vu0_micro"
        elif unit == "0" and kind in {"macro", "cfc2", "ctc2"}:
            field = "vu0_macro" if kind == "macro" else "vu0_control"
        elif unit == "1" and kind in {"retired_nop", "retired_world", "retired_world_clipped"}:
            continue
        elif unit == "1" and kind in FIXED_RETIREMENTS:
            if int(address, 16) != FIXED_RETIREMENTS[kind]:
                raise ValueError(f"invalid address {address} for {kind}")
            continue
        else:
            raise ValueError(f"unknown VU census kind {kind} for VU{unit}")
        counts[field] += int(count)
    if ended != {"0", "1"}:
        raise ValueError("missing VU census shutdown summary")
    for field in CENSUS_FIELDS:
        if counts[field] != audit[field]:
            raise ValueError(f"{field} differs between census ({counts[field]}) and audit ({audit[field]})")
    if audit["vu0_mlist"] > audit["vu0_micro"]:
        raise ValueError("VU0 matrix-list count exceeds all VU0 micro dispatches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-vu-census", action="store_true", help="Also reconcile the VU census with the audit")
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
        lines = log.splitlines()
        summaries = [i for i, line in enumerate(lines) if line.startswith("[r7-audit]")]
        if len(summaries) != 1:
            print(f"{path}: FAIL: expected one R7 shutdown summary, found {len(summaries)}")
            failed = True
            continue
        summary_index = summaries[0]
        summary = lines[summary_index]
        if not summary.startswith(PREFIX):
            print(f"{path}: FAIL: expected R7 shutdown summary v=2 (all VU0 counters required)")
            failed = True
            continue
        match = SUMMARY.fullmatch(summary)
        if match is None:
            print(f"{path}: FAIL: malformed R7 shutdown summary")
            failed = True
            continue
        try:
            audit = dict(zip(FIELDS, map(int, match.groups())))
            if args.check_vu_census:
                if any(line.startswith("[r7-vu]") for line in lines[summary_index + 1:]):
                    raise ValueError("VU census record after the R7 shutdown summary")
                check_vu_census(lines[:summary_index], audit)
        except ValueError as error:
            print(f"{path}: FAIL: {error}")
            failed = True
            continue
        if args.check_vu_census:
            print(f"{path}: VU census reconciles with the shutdown audit")
        nonzero = [f"{key}={value}" for key, value in audit.items() if value]
        if nonzero:
            print(f"{path}: FAIL: {' '.join(nonzero)}")
            failed = True
        else:
            print(f"{path}: PASS: zero in all R7 runtime counters")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
