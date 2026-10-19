#!/usr/bin/env python3
"""Adapt the Battle smoke journey to a native map and a longer chase test.

Arguments: base pad timeline, track display name, frame bound, pad index,
optional scenario (weapon selects held/release/repress native reload input).
Menu offsets are specific to the unlocked SLUS_209.10 Action multiplayer menu.
"""
import bisect
import sys

base, track, bound, pad = sys.argv[1:5]
scenario = sys.argv[5] if len(sys.argv) > 5 else "mixed"
bound, pad = int(bound), int(pad)
steps = {"Dennison": 0, "Home": 6, "Mankato": 8, "Woodly": 9, "Anoka": 10}
if track not in steps:
    raise SystemExit("Unsupported scripted map: " + track)
# Native Action multiplayer traversal measured from the rendered menu.
# Woodly is nine RIGHT taps from the initial choice; ten wraps to Anoka.
shift = steps[track] * 40
events = {}
for entry in base.split(","):
    frame, value = entry.split(":")
    frame, value = int(frame), int(value, 16)
    # Home's Battle is last in its sixteen-contest menu: one UP wraps to it.
    if pad == 0 and track == "Home" and 4030 <= frame <= 4280:
        continue
    events[frame + (shift if frame >= 3500 else 0)] = value
spans = []
if pad == 0:
    spans += [(3300 + i * 40, 3310 + i * 40, 0x20) for i in range(steps[track])]
# Keep both humans requesting native respawns in longer runs.
for frame in range(11900 + pad * 50 + shift, bound - 100, 600):
    spans += [(frame, frame + 10, 1), (frame + 150, frame + 160, 0x100)]
if track != "Dennison":
    # Move the human target across the map, with alternating turns. This is
    # repeatable stimulus, not a guarantee that the human avoids every wall.
    for frame in range(6800 + pad * 180 + shift, bound - 500, 1600):
        spans += [(frame, frame + 300, 0x4000),
                  (frame + 300, frame + 390, 0x4000 | (0x20 if (frame // 1600 + pad) % 2 else 0x80))]
if scenario == "weapon":
    # Native reload evidence: hold through cooldown expiry, release, repress.
    # Stop unrelated driving/firing/respawn stimuli for both human pads.
    events = {frame: value for frame, value in events.items() if frame < 5000}
    events[5000] = 0
    spans = [] if pad else [(5200, 5550, 0x100), (5580, 5590, 0x100)]
times = sorted(events)
points = sorted(set(times) | {p for start, end, _ in spans for p in (start, end)})
output = []
previous = None
for frame in points:
    i = bisect.bisect_right(times, frame) - 1
    value = events[times[i]] if i >= 0 else 0
    for start, end, buttons in spans:
        if start <= frame < end:
            value |= buttons
    if value != previous:
        output.append(f"{frame}:{value:04x}")
        previous = value
print(",".join(output))
