# Test Drive: Eve of Destruction · PC and Mac

**Take the dirt oval at night, or trade paint under the floodlights.** The 2004 PS2 demolition racer runs as a native app on Apple Silicon and Windows x64. Bring your North American game disc, plug in a controller and pick your first event.

[Download the Mac and Windows preview](https://github.com/catsandsoup/eod-recomp/releases/tag/preview-2026-11-08-couch) · [Host and guest guide](docs/ONLINE_PLAYER_JOURNEYS.md)

![Two cars collide under the floodlights during a night demolition derby](docs/img/night-battle-demolition-derby.jpg)

Play a local split-screen Battle with a friend beside you, or try the experimental online lobby with a whole screen to yourself. Choose the original 4:3 framing or widescreen, adjust the internal resolution, and keep the game's physics while you tune the picture for your machine.

The public download uses your own retail content. The personal integrated build pictured below also showcases bonus cars and event mods derived locally from the owner's disc.

## Features

### Play your way

* **Controller first.** Xbox and PlayStation controllers work out of the box, with analog triggers for gas and brake. Keyboard and mouse work too.
* **Hot-plug 2-player split screen.** Plug in a second pad whenever you like and the game is ready for two.
* **Widescreen and higher resolutions.** Sharper than the PS2 ever managed, in 4:3 or true widescreen.
* **Everything unlocked outside Career.** Jump straight into any car and any map in the other modes.

### Battle mode everywhere

![Two-player split screen battle against eight AI cars on a Test Drive: Eve of Destruction dirt map](docs/img/2-player-split-screen-battle.jpg)

* **Battle on every map.** Pick 4 AI or 8 AI opponents, with mixed car classes.
* **Battle for two.** 2-player split screen offers 1v1, 4 AI and 8 AI.
* **Battle events in Career.** Fresh events appear in the Career event list.
* **Every rival has a name.** Take out an AI driver, or get taken out, and the game tells you exactly who it was.

![Battle message reading "You were killed by Kevin Valinski."](docs/img/battle-kill-message-named-rival.jpg)

![A school bus T-bones the Phantom under a floodlit suspension bridge in an 8-AI night battle](docs/img/school-bus-battle-floodlit-bridge.jpg)

![Battle event list showing 1v1, 4 AI and 8 AI battles on the Woodly demolition map](docs/img/battle-event-list.jpg)

### Bonus cars

* **The Phantom.** A mean, black-and-white new ride for the demolition derby.
* **A NASCAR stock car.** Take a Cup-style racer through the mud. It crumples, sheds bumpers and panels, and casts a proper shadow, just like the original cars. More are on the way.
* **Special vehicles in Career.** The School Bus, Hearse, Ambulance, Taxi, Mail Van and Police Car are now stocked at the Career car dealer and eligible for Career events.

![The bonus #24 NASCAR stock car trading paint with a station wagon in a night battle](docs/img/nascar-stock-car-night-battle.jpg)
![The bonus Phantom car lining up at the start of a race](docs/img/phantom-bonus-car.jpg)

### Career, wide open

* **Every shop and event open from day one.** Upgrades, Time Challenges, the Highway, Troy's Place and all 25 Eves are open right away.
* **The dealer sells every car.** Every visit to the auto shop brings in a fresh line-up until you've seen the entire roster — including the Phantom, the NASCAR stock car and all six special vehicles — and swap or trade in cars for a 100% refund so you can try anything at no loss.
* **Never broke, never locked out.** Career cash stays topped up to at least $10,000 in free roam while winnings still add on top, and your rank stays at **1st** without ending the Career.
* **Killable challengers.** Highway and Malt Shop challengers in Career free roam no longer stop taking damage at 90% — you can wreck them outright. Every Career unlock can be toggled live in the **Mods** tab.

### A modern menu that looks like the original

Press **Start + Select** on a controller (or **F1** / **Cmd+,** on a keyboard) at any time to open the in-game menu, styled like the original game. Tabs for **Graphics, Mods, Settings, Inputs and Online** let you change things without leaving your race.

![In-game menu Graphics tab with render scale, aspect ratio and fullscreen options](docs/img/in-game-menu-graphics.jpg)

### One lobby, another race

The host picks the track and event. Each driver chooses a car and paint, sees their friend's choice, and marks Ready. End Race returns the party to Online, where the same Garage Code and roster are waiting for the next start.

![Anoka night race captured from the host's game during a two-player rematch test](docs/img/online-anoka-race.jpg)

* **Stay on the couch.** Open the menu with Start + Select. Use the shoulder buttons to reach Online, the D-pad to navigate, Cross/A to choose and Circle/B to go back. A guest can type the Garage Code and confirm on the controller. A discovered LAN listing skips typing.
* **See who made it in.** The code and player list stay visible while the host browses settings. Once racing, Start opens the live menu. End Race keeps the party together; Close Lobby ends the session.
* **One car per player, one screen each.** A Mac and the Windows executable running under CrossOver on the same Mac completed two consecutive races in both host/guest configurations. Native Windows GPU performance and internet stability still need testing.

![After ending the race by controller, the host sees the Anoka settings, Garage Code, both Hurricane drivers, readiness and voice controls](docs/img/online-rematch-lobby.jpg)

**Preview limits.** The lobby admits 16 occupants including the host. An event's available grid seats determine how many can actually drive, Battle has at most 10 total cars, and voice supports the first four player IDs. Two-human rematches are exercised. Long races have shown remote-car updates stall. [Capacity, lifecycle and test coverage](docs/ONLINE_PLAYER_JOURNEYS.md)

### Your own soundtrack

Drop your own **MP3, AAC, FLAC or WAV** files into a folder and they play alongside the original songs, just like custom soundtracks on the Xbox version. Choose which ones are on from the Mods tab, and the game remembers your picks next time you play.

![Mods tab of the in-game menu showing custom music and bonus content toggles](docs/img/in-game-menu-mods-custom-music.jpg)

### Dirt, fences and floodlights

![Night race on a floodlit dirt oval in Test Drive: Eve of Destruction](docs/img/night-race-dirt-oval.jpg)

## Coming soon

No dates, just where we are headed:

* Broader internet, physical microphone and multi-machine testing for the Garage Code preview
* **Higher draw distance and more detail**
* **More cars on track**
* **Texture packs**, including 4K
* **The full NASCAR roster**
* **Mod support** for custom cars and tracks

## Getting started

1. **Get the preview.** Download the archive for your platform from [catsandsoup/eod-recomp Releases](https://github.com/catsandsoup/eod-recomp/releases). On Mac, open **Eve of Destruction.app**. On Windows x64, extract the ZIP, put your extracted disc files beside **eod.exe** or pass `--data <disc-folder>`, and use **eodsetup.exe** for settings.
2. **Bring your own game.** You need your own legally obtained copy of Test Drive: Eve of Destruction (North American PS2 version).
3. **On Mac, pick your disc image.** On first launch the app asks you to choose your game's disc image (or the disc's folder). It checks that it is the right game, copies what it needs, and shows you progress.
4. **Play.** That is the whole setup. Next time, it launches straight into the game.

<details>
<summary>Build it yourself</summary>

Want to build the app from source? See the [macOS build guide](docs/MACOS.md).

</details>

## Frequently asked questions

**Is this an emulator?** No. The game's code has been translated ahead of time into native code, so it runs as a regular app.

**Does it include the game?** No. You must own the game and supply your own copy.

**Which platforms?** Apple Silicon macOS and Windows x64 preview builds are available. Windows uses Direct3D 11. Native Windows hardware benchmarks are pending.

## Legal, credits and licences

This is an unofficial fan project. It is **not affiliated with or endorsed by Atari, Monster Games or the owners of any trademarks** mentioned here. Test Drive: Eve of Destruction and all related names belong to their respective owners.

**No game data is included.** You must own the game to use this port.

Credits: this port builds on [eod-recomp by pareidolico](https://github.com/pareidolico/eod-recomp), the original static recompilation of the game for Windows, which takes its approach from projects such as N64Recomp and Zelda64Recomp. The menu system uses [Dear ImGui](https://github.com/ocornut/imgui) (MIT licence) and the Mac version uses SDL3 and Vulkan through MoltenVK. Thanks to everyone who built those projects.

*Keywords: Test Drive Eve of Destruction PC, Eve of Destruction Mac, TD EOD recomp, PS2 demolition derby port, native port, Apple Silicon, static recompilation.*
