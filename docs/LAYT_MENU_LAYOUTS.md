# LAYT menu layouts (`*.lyt`)

200 members across UI3D, UIACTION, RACEDAY, PAINTKIT, COMMON, UIEDIT and RACE `.RES` and
`HOMEBASE.TRK`: every menu, dialog and overlay screen is one widget tree stored in one file
(for example `COMMON.RES#yesnocan.lyt`, 1,952 bytes; `UIACTION.RES#main.lyt`, the main menu;
`HOMEBASE.TRK#tumaindrive.lyt`, 5,032 bytes). Decoder: `tools/formats/layt.py`
(view title "Menu layout"). Tests: `tools/tests/test_formats_layt.py`.

Every file is `0x68 + N * 0x134` bytes: a fixed header, N widget records and a final end-marker
record. The loader never parses the file; it keeps it in memory and walks it in place.

## Header (0x68 bytes)

| Offset | Size | Field | Evidence |
|---|---|---|---|
| 0x00 | u32 | flag, 1 in 19 files; no reader found | `0x00270430`, `0x0027C980` |
| 0x04 | char[32] | window background: a `.img` picture, `dialog` (stock frame, window flag set in `0x0027C980`) or empty. A `.img` name is loaded with `ImageLoad 0x00349078`; a miss prints "UI: missing background" | `0x00270430` -> `0x0027C980` (`0x0027CA10..0x0027CA74`) |
| 0x24 | u8 | 1 = also build the shared `back.lyt` widgets (loaded with `0x00270F10`) | `0x00270430` at `0x00270720`, `0x002708C0` |
| 0x25 | u8 | 1 = play the camera track named at 0x28 (`CameraDirectorPlayTrack 0x001D2DC8`; outside 3D menu mode it prints "UI: Not in Lite Mode, can't use camera") | `0x00270430` at `0x002704C8` |
| 0x28 | char[60] | camera track name (`main`, `buycar`, `maindrive`, ...), empty in 171 files | same |
| 0x64 | u32 | build address, overwritten with the address of the first widget | `0x00270F10` at `0x00270FA4` (`layout + 0x68`) |

`LayoutLoad 0x00270F10` calls `ResourceGet 0x00132638` with type `'LAYT'` and version 5
("Wrong layout version in %s: %x != %x" on mismatch) and panics "Can't get layout %s" when the
file is missing. It then points `+0x64` at `+0x68` and returns. The version is stored in the
archive directory, not in the payload.

## Widget record (0x134 bytes, stride confirmed at `0x00270FD4`)

The list ends at the first record whose first word is 0 (`0x00270FC0`). The builder
`0x0026D5E8` (called per record from `0x00270430` at `0x00270904`/`0x00270954`) switches on
`kind - 2` through the jump table at `0x004101B0` (kinds 2 to 51; other values fall through
and are skipped). Each case allocates a widget object, hands it the record fields, and
registers it with `0x0027B9F0` -> `0x0027E418` (record +0x88).

| Offset | Size | Field | Used by (examples) |
|---|---|---|---|
| 0x00 | u32 | widget kind | jump table `0x004101B0` |
| 0x04 | u32 | style: variant, font size class or art mode; bit 31 set on some text labels (read as the sign bit by `0x00280CE8` at `0x00280DA8`) | `MenuBtn` case `0x0026DCFC`, `ArtWidget` `0x0026E1EC`, text `0x00280D98` |
| 0x08 | i32 | x, pixels from the left | every case; base widgets `0x00289D40`, `0x0028C510` pass x, y, x+w, y+h to the rectangle setter `0x002912F8`; text builder `0x00280CE8` turns x, y into 3D coordinates `x - 320`, `240 - y` |
| 0x0C | i32 | y, pixels from the top | same |
| 0x10 | i32 | third coordinate passed with x and y (0 for most; 89, 90, 100 on full-screen pictures and game-drawn areas) | `0x00290D88`, `0x00280210`, `0x002912F8` |
| 0x14 | i32 | width | kinds in the table below marked W/H |
| 0x18 | i32 | height | same |
| 0x1C | f32 | text size in pixels on kinds 28, 31 and 43 (18 to 40); build noise elsewhere | `0x00280DE4` (`lwc1 0x1C`) |
| 0x20 | char[32] | first string: widget name (kind 2, 3, 5 to 27, 33, ...), font for kind 28 and 31 | `0x0026D3D8` name lookup |
| 0x40 | char[68] | second string: label text, picture/art/video/font name | `0x001347D0` text lookup, `0x003A7E80` ".img" test |
| 0x84 | u32 | text effect flag (2 on 239 labels) | `0x00280DDC` |
| 0x88 | i32 | page group: -1 none, 0 to 31 group number | `0x0027E418` stores `1 << n` at widget +0x240; `0x0027E5A0` applies a method to the widgets whose bit matches |
| 0x8C | u32 | interface cue number (0, 1, 2, 3, 10) stored at widget +0x244 and raised through `0x002A0BA0` | base widgets (`0x00289D40` at `0x00289D58`) |
| 0x90..0xB7 | 40 B | colour words (ARGB, 0xFFFFFFFF common), text outline floats, per-kind options | text `0x00280DCC`, `0x0026E1B8` (0x9C, 0xA0), `0x00289D40` (0x9C, 0xA4) |
| 0xB8..0x133 | 124 B | identical style defaults in every widget of a kind (colour table, build addresses); not read by the builders traced | not read |

The end marker (last 0x134 bytes) starts with 0 and holds build-tool scratch (`0xE3` fill and
a stale copy of the background name). It is never read.

### Coordinate space

640 x 480, origin top left. Evidence: the text builder subtracts 320 and from 240 at
`0x00280EC0`/`0x00280ED0`; full-screen widgets (kind 41, 33 `rstd_frame.art`, 26 `fader`) are
641 x 481 (one pixel past the 640 x 480 edge) with x = y = -1. Not 640 x 448. A window with a
`.img` background is centred when the caller passes -999 (`0x002705EC..0x0027064C`, window
size = picture size); dialogs are built that way, so their widget coordinates are relative to
the picture. Pictures drawn by kinds 33 and 41 are stretched to the widget rectangle (the main
menu picture is 512 x 480 and its widget is 641 x 481).

### Text and name resolution

- First string, kinds with a name: `0x0026D3D8(dialog, name, type)` searches the screen's
  descriptor table (20-byte entries, name at +0, type at +4, value at +8, terminated by type
  0x7B, searched case-insensitively with and without a leading `#`). The screen's code fills
  this table; a hit gives it control of the widget (its text, picture, value). Renaming a
  widget therefore breaks the link, so the decoder keeps names read-only.
- Second string on text kinds: starting with `#` means the descriptor supplies the text
  (`0x0026D8E0`..`0x0026D9D4`, `CustomMulti`/`CustomText`); otherwise `LangGetString 0x001347D0`
  returns the language-file text for that key, or the string itself when no key matches
  (`docs/LANG_GAME_TEXT.md`). Across the 200 layouts: 156 text strings are language keys
  (`Generic:ACCEPT`, `UIMain:RACE`, `Money:`...), 729 are `#` bindings, the rest are literal
  English or type labels (`Custom`, `Tab Box`) that are never drawn.
- Fonts: kind 28 (`0x0026DA1C`..`0x0026DB34`): when the descriptor's entry ends in `.gtx` the
  text is drawn as glyph art (`gText`); otherwise `CustomText 0x00280CE8`. The first string
  holds the font file (`donfont.gtx`, `arenacb.gtx`, `arialbd.gtx`; older screens `arial18.fnt`).
- Pictures: `0x003A7E80` tests the ending `.img`; `.img` goes through `ImageLoad 0x00349078`
  (IMAG, `docs/IMAG_INTERFACE_IMAGES.md`), `.art` is a TEX texture, `.gtx` a font texture.
  In the same archive 475 of 661 `.img` names, 736 of 827 `.art` and 1,498 of 1,839 `.gtx`
  names resolve; big backdrops such as `main.img` live in `UIBIG.RES`. The rest are resolved by
  the resource registry at run time (not statically).

## Widget kinds

Counts are over the 200 retail layouts. "W/H" = width and height are read.

| Kind | Count | Name | W/H | Builder | Strings (first / second) |
|---|---|---|---|---|---|
| 2 | 177 | Image button | yes | StyleImage/IButton/MenuBtn `0x0026DBF8` | name / label or `#name` |
| 3 | 8 | Picture button, retired | | prints "BButtons aren't supported anymore", skipped (`0x0026DF34`) | name / `b_*.img` |
| 5 | 14 | Static text | | StaticText `0x0026D694` -> `0x0028FFE8` | name / text |
| 6, 7 | 4, 4 | Decimal / whole-number readout | | Numeric `0x0028F848`, IntNumeric `0x0028EC18` | name / printf format |
| 8 | 0 | Arrow number selector | yes | IntArrowBtn/IntSlider `0x0026E490` | |
| 9 | 77 | Setting stepper | yes | SpinCtl `0x0026EB58` | name / label |
| 10 | 22 | Choice list | yes | Multi `0x0026F044` | name / label |
| 11 | 20 | Text entry | yes | Input `0x0026F3B8` -> `0x0028E1C0` | name / text |
| 12 | 0 | Controller detector | | CDetect `0x0026FBA4` | |
| 13, 14, 15 | 0, 0, 3 | Check box, radio button, picture radio | | `0x0026E21C`, `0x0026E2C8`, `0x0026E3B8` | name / label or `.img` |
| 16, 27 | 7, 0 | Tab button, picture tab | | TabButton `0x0026F1F0`, BTabButton `0x0026F2E0` | name / label |
| 17 | 3 | Stamp roll, retired | | prints "Stamprolls are not supported anymore" (`0x0026DFBC`) | name / `.img` |
| 18 | 8 | Slider | yes | Slider `0x0026E6F4` | name / label |
| 20, 21 | 2, 0 | Vertical, horizontal scroll bar | yes | `0x0026F7A0`, `0x0026F99C` | |
| 23 | 9 | List box | yes | ListBox `0x0026F66C` | |
| 26 | 187 | Game-drawn area | yes | Custom `0x0026F490` (the screen's code draws into it: car model, colour grid, `fader`) | name / `Custom` |
| 28 | 1,421 | Text label | | gText/CustomText `0x0026D8CC` | font / text key, literal or `#name` |
| 31 | 85 | Table column | yes | Column `0x0026DB44` -> `0x0028FB50` | font / column name |
| 33 | 1,964 | Art | yes | ArtWidget `0x0026DFCC` -> `0x00280210`; `.img` goes to `0x00290D88` | label / art or picture name |
| 34 | 7 | Track backdrop | | BackWidget `0x0026DF4C` | / `trk.tex` |
| 36, 37 | 0, 1 | Bottom bar, bar tab | | `0x0026FC68`, `0x0026FD48` | |
| 39 | 5 | Movie | yes | `0x0026FEBC` -> `0x0028C688` | name / `.m2v` |
| 41 | 41 | Full-screen picture | yes | `0x0026FF84` -> `0x0028C510` | / `.img` |
| 42 | 39 | Fading picture button | yes | FadeButton `0x0026DE2C` | name / `.img` |
| 43 | 31 | Message text | yes | `0x0026FFCC` -> `0x00287E68` | `#message` / font |
| 44 | 14 | Button list | yes | `0x0027005C` -> `0x00288928` | name / `ButtonList` |
| 45 | 3 | Rotating picture | yes | `0x002700F0` -> `0x00289730` | name / `.img` |
| 46 | 157 | Controller button glyph | yes | `0x00270138` -> `0x00289D40` (sheet name hard-coded in the builder, `ps2butns.img` or `ps2tbutns.img`; field 0x9C, 0 to 3, picks the glyph: 0 and 1 from the first sheet, 2 and 3 from the second; the second string is ignored) | name / `ps2butns.img` |
| 48 | 38 | Tab list | yes | `0x002701C8` -> `0x0028BB78` | `tablist` / title |
| 49 | 17 | Choice from list | yes | `0x0026EF58` -> `0x0028E6C8` | `#name` / `Multi` |
| 50 | 3 | Volume slider | yes | `0x002702F0` -> `0x0028DC60` | name / `sslider` |
| 51 | 41 | Tab box | yes | `0x002702A8` -> `0x0028BD08` | `Tab Box` |
| 35, 38, 47 | 0 | unused in retail | | `0x0026F5D8`, `0x0026FE28`, `0x00270180` | |

Kinds 4, 19, 22, 24, 25, 29, 30, 32, 40 have no builder and are skipped.

## What the decoder edits

Editable (same-length or fitting values, plain-sentence rejections otherwise): x and y of every
widget except retired kinds 3 and 17 (-2048 to 2048), width and height on kinds that read them
(0 to 2048), fonts and pictures (must fit the 31 or 67 character slot and keep the `.fnt`,
`.gtx`, `.img` or `.art` ending), visible text that is a key or literal (not `#` bindings), and
the window background (`.img`, `dialog` or empty). Names, bindings, style, page group, cue,
colours and build values are shown read-only. No-edit `encode` returns the input bytes;
an edit changes only the edited bytes (a shortened string rewrites up to its old
terminator).

The Creation Kit view carries the table preview (`#`, Type, Name, X, Y, W, H, Image/Font,
Text) under `table` and a 640 x 480 schematic PNG as the preview: widget rectangles by kind
colour with names, retired widgets in grey, referenced `.img` pictures drawn in place from the
same archive (then `COMMON.RES`, then `UIBIG.RES`). `.art` and `.gtx` are drawn as labelled boxes.
In-animation offsets (doors at x = -123, menus that slide in) are shown as stored.

## Observed in the retail data

- `yesnocan.lyt` is not what the game opens for Yes/No prompts: its three buttons are kind 3
  (retired) and its background `dialog2.img` is not in COMMON.RES. Active prompts are the
  `yesno.lyt` copies in UIACTION, UI3D, RACEDAY and HOMEBASE.
- `main.lyt` (main menu, UIACTION): full-screen `main.img`, four image buttons `arcade`,
  `career`, `load`, `options` with keys `Generic:ACTION`, `Generic:CAREER`, `Generic:LOAD`,
  `UIMain:OPTIONS`, a controller glyph and `Generic:ACCEPT`, a game-drawn `fader` and the
  `rstd_frame.art` border. It matches the state machine in `docs/UI_MENU_ENGINE.md` section 1.3.
- `tumaindrive.lyt` (HOMEBASE.TRK, career hub "Drive" tab): tab list, buttons `switch`
  and `repair` (literal `Switch Cars`, `Repairs`), tab box with `Money:`, `Rank:`, `Goal:`,
  `Car:` and the `#money`, `#rank`, `#goal`, `#chassis` bindings, game-drawn `car_bar`.

## Unknowns

- Header flag at 0x00 (no reader found).
- Meaning of the style word at +4 per kind, the third coordinate at +0x10 (draw order is a
  guess), the cue at +0x8C (which sound or event each number raises), and the field-by-field
  layout of 0x90..0xB7 (colour words and outline floats); they are exposed read-only or as one
  block.
- Whether the 0xB8..0x133 defaults are read by any widget (none of the traced builders do).
- Screens the code opens by name beyond the ones in `docs/UI_MENU_ENGINE.md` (the summary
  derives the rest from widget names and text keys).
- The layout version 5 is checked by the resource loader from directory data; no layout
  payload contains it.

## Decisive native test

Move the `career` button of `UIACTION.RES#main.lyt` 40 px left (`w2.x`, 352 to 312), install
the edit in a separate test profile and open the main menu at maximum quality: the label and
highlight must move together and the other three buttons must stay. Then raise the width of
`arcade` (`w1.w`, 114 to 160) and check that the highlight follows. Capture both screens.
