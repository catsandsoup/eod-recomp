# LANG: game text tables (`*.lng`)

Every string the game shows lives in one language table. Decoder:
`tools/formats/lang.py` (`FOURCCS = ("LANG",)`); view contract:
`docs/FORMAT_VIEW_CONTRACT.md`. Tests: `tools/tests/test_formats_lang.py`.

## Retail members

| Archive | Member | Size | Entries | Used by the game |
|---|---|---|---|---|
| `DATA/RES/LOCALE.RES` | `usenglish_lang.lng` | 38 751 B | 877 | Yes: loaded at start-up |
| `DATA/RES/GENESIS.RES` | `english.lng` | 13 635 B | 325 | No reference found in the executable |

Both use the same layout. All 325 keys in `english.lng` also appear in
`usenglish_lang.lng`; the executable names only
`*locale.res/usenglish_lang.lng` (and `*locale.res/usenglish_units.val`).

## File layout (little endian)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 8 | Language name, NUL padded (`English`, `english`). Not read by the game |
| 0x08 | 4 | Build version `0x7E4` (2020). Not read by the game |
| 0x0C | 52 | Thirteen u32 words of build-tool bookkeeping (pointer values, constants 1 and 4, zeros). Not read by the game; one word differs between the two files |
| 0x40 | 4 | Entry count N |
| 0x44 | 8 x N | Index: per entry `u32 key offset`, `u32 text offset`, from the start of the file |
| 0x44 + 8N | rest | String pool: 2N NUL-terminated strings, packed with no gaps and no sharing |

Index order is case-insensitive alphabetical by key. Pool order is not the
index order in `usenglish_lang.lng` (the pool follows the source order); the
decoder and encoder keep both exactly as shipped. Keys are `Group:Name` paths
(`Main_Menu:SingleRace`, `Career:Credits:RacePrize`). One retail key appears
twice (`Tracks:mankato:Description`); lookups return the first.

## Text encoding and codes

- Latin-1, one byte per character. The only byte above 0x7E in retail is
  0xF6 (`Vikören`, driver name).
- `\n` (0x0A) is a forced line break; it appears in two-line button labels
  such as `QUICK\nRACE` and `MULTI\nPLAYER`. No other control bytes occur.
- printf placeholders occur in two retail strings only: `Race Prize #%d` and
  `Season Prize #%d`. The game passes an integer; changing or dropping the
  placeholder is rejected by the editor.
- No colour or button-glyph escape codes exist in the table.

## Loader and lookup (evidence)

Addresses are in `SLUS_209.10`; use `python3 tools/ee_dis.py -f <addr>`.

| Address | Role |
|---|---|
| `0x00134048` | Language init. Fills the single language slot (display name `English (US)`, path `*locale.res/usenglish_lang.lng`, units path `*locale.res/usenglish_units.val`), sets the slot count to 1, then calls the loader |
| `0x001342A8` | `LangLoad(slot)`. Calls `ResourceGet` (`0x00132638` / `0x00130D90`) with type `LANG` (`0x4C414E47`), copies the member into the static language state (`0x0043A320 + 0x30C`) with `memcpy` (`0x003A5E1C`), then adds the load address to every key and text offset (loop over the count at blob +0x40, entries at +0x44 / +0x48). Messages: `Can't load %s`, `Bad version on %s` |
| `0x00134708` | `LocalizedStringInit(ls, key)`: stores the key pointer, clears the cached text, tags the load generation. 133 call sites, for example the main menu (`MainMenuUpdate` `0x002A1298` passes `Main_Menu:SingleRace`, `Main_Menu:Career`, ...) |
| `0x00134620` | `LocalizedStringResolve(ls)`: linear search of the key column with `stricmp` (`0x003A72D8`, case-insensitive); the cached text is the entry's text, or the key itself when absent |
| `0x001347D0` | `LangGetString(key)`: same search, returns the text, or the key when absent (4 direct callers, for example `0x0026D860`) |
| `0x00134728` | `LangHasKey(key)` |
| `0x001346F0` | Bumps the load generation so cached strings re-resolve |

Keys are literal strings in the executable (for example `Main_Menu:SingleRace`
at `0x00414058`), so keys cannot be renamed or added to the table without a
matching code change; only the text can be edited.

## Limits

- There is no length check in the loader: the member is copied whole. Static
  headroom after the language buffer is about 50 000 bytes from the start of
  the copy (next statically referenced data at `0x00446980`), so roughly 11 KB
  of growth over retail. This is derived from the executable, not from a run;
  the encoder refuses files over 50 004 bytes.
- Per-screen buffers and widths (button labels, dialog lines) were not traced
  for each of the 133 call sites. Long text may be clipped or overflow its
  widget. Test on screen.
- The font's coverage of accented Latin-1 characters beyond those in retail
  text has not been checked.

## Editing

Each entry is one editable field `string.<index>`; the key is shown in the
label and cannot be changed. On save the encoder rewrites the pool in its
original order, recomputes every offset, and leaves the header bytes and the
entry count untouched. An unedited save is byte-identical. Edits are rejected
when the text is not representable in Latin-1, contains a control character
other than the line break, contains a NUL, or changes the order or kind of
printf placeholders.

## Packaging

An edited table is shipped as a native member replacement
(`docs/CREATION_KIT_TEXTURE_CONTRACTS.md` section 5): a `native` entry for
archive `DATA/RES/LOCALE.RES`, member `usenglish_lang.lng`, with the source
archive and member hashes of the retail file.
