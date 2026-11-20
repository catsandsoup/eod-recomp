# EOD Creation Kit — Development Preview

Explore the world of **Test Drive: Eve of Destruction**, customise supported game assets, and test your changes in a separate game session.

- Browse vehicle and track archives with texture and recovered 3D previews.
- Edit supported vehicle settings and native textures.
- Export textures as PNGs, edit them in an image editor, and import them at their original dimensions.
- Save mods separately from your original game files.
- Launch saved mods in the included test runtime.

## Requirements

An Apple Silicon Mac and your own extracted North American PS2 game disc. The download includes the asset-processing runtime. You can open the app directly from Finder without installing Python or developer tools.

<!-- macOS minimum -->

## Get started

Extract the ZIP, move **EOD Creation Kit Development.app** to Applications, and open it from Finder. Choose your extracted game folder with **Game Folder**. Open or create a project, edit a supported asset, then use **Save & Build** and **Run Mod in Game**.

## Building the preview

Build with Xcode and an Apple Silicon-capable Python.org Python 3.10 or newer. On the build Mac, run `python3 tools/package_creation_kit_release.py --game-executable /path/to/eod --output /path/to/new-release-folder`. The packager uses the current Python.org framework in `/Library/Frameworks/Python.framework/Versions/Current`. Use `--python-framework` to select a different installed framework version. It produces a locally signed app and ZIP with the included game runtime and its libraries. Supply the extracted game data at first launch.

## Preview limitations

This is an early editing toolkit. Some asset types and model components remain unsupported. Texture imports retain the original dimensions and palette limits; 4K, PBR, HDR, arbitrary model import and live world editing are not available. This download is for macOS Apple Silicon; a Windows editor is not included.

## New in 0.3.2

Bus texture edits now appear in the 3D preview. Saving a Bus body texture also updates the default race paint used by the test session.
