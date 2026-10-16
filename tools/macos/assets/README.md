# App icon source

`AppIcon.png` is an AI-generated fan launcher mark made for this fork on
2026-10-16 with Codex's built-in image generator. It is not official game artwork.
The game title belongs to its respective owners. No disc artwork was extracted.

Generation brief: square macOS game icon, exact large TDEOD lettering and smaller
EVE OF DESTRUCTION, distressed steel, orange/red demolition-derby styling,
charcoal rounded-square badge, checkered flags and a derby car silhouette.

`tools/macos/bundle.py` uses macOS `sips` and `iconutil` to create all standard
16–1024 pixel representations in a temporary directory, then signs the bundle.
The PNG is the editable source; generated `.icns` stays in the local app.
