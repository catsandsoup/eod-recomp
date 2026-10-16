# Keyboard context in SLUS_209.10

The SDL Racing keyboard preset reads actual game state each pad poll. It does
not infer game state from a frame counter, screenshots, car speed or controller
activity. The Original preset retains the configured PS2 key bindings. Gamepad
input and its Original/Modern selection are independent.

| State | Target producer / consumer evidence |
| --- | --- |
| Driving loop active | `0x00114020` writes `1` to `0x00437BB8` at `0x00114058`, repeats the actual race/update loop, and clears it at `0x001141BC` before returning. Its debug message at `0x00114184` is `game_loop: The Race is over`. |
| Modal menu active | `0x0027C508` saves the previous `0x004A1AD4`, writes its window argument there at `0x0027C5DC`, dispatches input to that window and restores the previous pointer at `0x0027C8A0`. Nested menus therefore keep the modal flag active, including menus inside the driving loop. |
| Title | `0x002BEC08` builds `start.lyt` and `press the START button`, stores its scoped stack pointer in `0x003D7BB8` at `0x002BEDFC` and clears it at `0x002BEE34`. Alternate title `0x002BEE70` similarly sets `0x003D7BBC` at `0x002BEEC0` and clears it at `0x002BEF50`. |

Priority is Title, then Driving if the driving loop is active and no modal
window is active, otherwise Menu. EE RAM is checked for null before reading
these fixed target-specific addresses (the standalone controller diagnostic
has no game RAM). `EOD_INPUT_LOG=1` prints context transitions and all four raw
state words even in headless runs with scripted pad input.

The pad-to-UI event chain confirms actual button semantics: `0x00332938`
extracts pad byte 3 bit 6 (Cross) into state offset `0x20` at `0x00332B44`,
and pad byte 2 bit 3 (Start) into offset `0x40` at `0x00332B68`.
`0x003338D0` edge-detects those states and emits UI key `0x0D` for Cross at
`0x00333A54`, and `0x113` for Start at `0x00333A68`.
The title registers hotkeys for both codes at `0x002BEC70`/`0x002BEC90`.
The movie input consumer `0x003673C0` accepts keys `0x1B`, `0x0D` and `0x113`
(`0x003673F4` through `0x00367420`), so Enter as Cross also skips movies.

Racing preset: menus use WASD/arrows to navigate, Enter accepts, Escape goes
back. Title Enter sends Start alone. Driving uses W/Up for Cross (gas),
S/Down/Space for Square (brake), A/D or Left/Right for steering, and Escape
for Start (pause). Enter has no driving action. Other configured keys retain
their PS2 bindings. Reserved Racing keys override conflicting PS2 bindings,
so Enter never sends Cross plus Start and Space never sends Circle in a race.
Opposite directions cancel to center. A key held across a context change
must be released before it can acquire a different action; this includes
Escape opening Pause, Enter leaving Title, W crossing from Menu into Driving,
and custom keys. Original bindings remain independent of game context.

`eod_keyboard_test` exercises these cases and layout config round trips.
The target state evidence above is disassembly evidence; actual full-game
context-transition and physical keyboard checks are separate integration
validation, not established by a pure mapper test.
