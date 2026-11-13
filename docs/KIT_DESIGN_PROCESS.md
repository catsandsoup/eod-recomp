# Creation Kit: how the interface is designed

A user interface is not drawn first. It is the visible end of a chain of decisions: who it is for,
what they are trying to get done, what they already know, what the product must and must not do,
how its content is organised, how people move and act in it, and only then how it looks and moves.
Skipping a link is how tools end up with "tiny dropdowns" nobody can click and pages nobody knows
the purpose of.

This document does that whole chain for the Creation Kit. Part A explains the kinds of thinking that
go into an interface and where each comes from. Part B applies every one of them to the Kit and
records the decision taken. `docs/KIT_UX_REDESIGN.md` holds the detailed heuristic review, job map
and screen specs; this document is the reasoning that sits above it, and wins where the two differ.
`docs/design/kit-prototype.html` is an early flow sketch; prototypes from here on are built in SwiftUI (Part B §12).

---

## Part A: the kinds of thinking behind an interface

Every established design method describes the same layers in different words. Four are worth knowing:

- **ISO 9241-210 (human-centred design)**: understand the context of use, specify requirements,
  produce design solutions, evaluate them against the requirements, and repeat.
- **Double Diamond (UK Design Council)**: *Discover* and *Define* the right problem (first diamond),
  then *Develop* and *Deliver* the right solution (second diamond). Each diamond first widens (look
  at many possibilities) then narrows (decide).
- **Elements of User Experience (Garrett)**: five planes, from abstract to concrete, each built on the
  one below: *Strategy* (why, for whom) → *Scope* (what it does) → *Structure* (how it is organised
  and behaves) → *Skeleton* (where things go on screen) → *Surface* (what it looks like). A problem
  found at the surface often sends you back down to scope.
- **Goal-directed design (Cooper)**: research → personas → scenarios → requirements → an interaction
  framework → refinement down to the pixel.

Combined, these give twelve kinds of thinking. Each answers one question; none can be skipped.

| # | Thinking | The question it answers | Typical methods | Plane |
|---|---|---|---|---|
| 1 | **Product strategy** | Why does this exist, for whom, and what counts as success? | Problem statement, success measures, constraints | Strategy |
| 2 | **User research** | Who are the users, what do they already know and use, in what situation? | Interviews, observation, competitor and analogue study, personas | Strategy |
| 3 | **Jobs and tasks** | What are people trying to get done, step by step? | Jobs-to-be-done ("When…, I want to…, so I can…"), task analysis, scenarios | Scope |
| 4 | **Mental models** | How do users *think* the thing works, and what words do they use? | Card sorting, vocabulary from comparable tools, Apple's mental-model principle | Structure |
| 5 | **Information architecture** | How is everything grouped, named, navigated and found? | Organisation, labelling, navigation and search systems (Rosenfeld & Morville) | Structure |
| 6 | **Interaction design** | What happens when people act, and what state is the system in? | Object-action model, state diagrams, direct manipulation, feedback, undo | Structure |
| 7 | **Platform conventions** | What does the operating system already teach users? | Apple HIG, first-party apps as reference, native components | Skeleton |
| 8 | **Layout and information design** | Where does each thing go, and what is seen first? | Wireframes, visual hierarchy, Gestalt grouping (proximity strongest), Fitts's law | Skeleton |
| 9 | **Content design** | What does each word say, and is it needed? | Labels, titles, empty states, error and recovery text | Skeleton / Surface |
| 10 | **Visual, motion and sound design** | What does it look, move and sound like, and does it carry the brand? | Colour, type, icons, motion with purpose, Reduce Motion | Surface |
| 11 | **Accessibility and inclusion** | Can everyone use it: keyboard, VoiceOver, colour blindness, larger text? | HIG accessibility, contrast, labels on every control | All |
| 12 | **Evaluation** | Does it actually work for real people? | Heuristic review, prototypes, usability tests with ~5 people per round, in-product measures | All |

Two sets of principles act as the yardstick at every layer:

- **ISO 9241-110 interaction principles**: suitability for the user's tasks, self-descriptiveness
  (the screen says what it is and what is happening), conformity with user expectations,
  learnability, controllability, use-error robustness, user engagement.
- **Apple's design principles** (WWDC "Essential Design Principles", HIG): *wayfinding* (where am I,
  where can I go, what will I find there), *feedback* (clear, timely, informative), *mental models*
  (meet the model people already have; changing it is costly), plus clarity, deference to content and
  consistency.

NN/G's ten usability heuristics are the review checklist used in `KIT_UX_REDESIGN.md` §1.

---

## Part B: the Kit, layer by layer

### 1. Strategy

**Problem.** Eve of Destruction's data is now fully understood by tools in this repository, but a
player who wants to change the game has to know archive names, file formats and command lines.
The Kit exists so a player can change cars, tracks, skies, Career and saves, see the result in the
game within seconds, and share it as one file.

**Who it is for, in order.**
1. The *Tinkerer* (largest group, and the project owner): plays the game, wants a change they can
   see ("drive the school bus in Career", "make this car faster", "sunset sky"). Thinks in Blender,
   Unreal, Xcode and regedit terms. Dislikes being asked questions; reads a missing button as a
   missing feature.
2. The *Texture artist*: exports, upscales, re-imports; works in an image editor beside the Kit.
3. The *Vehicle modder* and *Track tweaker*: deeper edits, Blender round trips, collision and props.
4. The *Distributor*: packages and shares.
5. The *Researcher*: wants raw bytes and evidence (served by the expert Game Files area).

**Success, measured.**

| Measure | Target |
|---|---|
| From "I want to change X" to seeing X in the game | under 60 s for a value or texture edit, with no game restart |
| Values shown by their raw file name only | none (done: every one of the 2,851 table key patterns has a plain label) |
| Main-thread stall while any job runs | under 50 ms |
| A job someone names but cannot find | none: every capability is visible, disabled with its reason if it can't run |
| Work lost to a crash or a game crash | none: autosave, game in a separate process |
| A shared mod a friend can install | one file, one double-click |

**Constraints that shape the design.**
- The game runs as its own process; a game crash must never take the Kit down.
- The game builds its virtual disc once at boot from the mod folder (`runtime/iop/stream.cpp`
  `build_disc`), which is why every change needs a restart today (section 6.4 removes that).
- The Kit cannot ship game art: anything visual from the game is rendered from the player's own
  disc on first run.
- Kit concurrency rule (AGENTS.md): the interface never waits on work.
- Product language rule (AGENTS.md): every line of copy identifies, explains a consequence,
  resolves uncertainty or helps recovery.

### 2. User research

**What we have.** The owner's direct feedback on Kit 0.4.1/0.5.0 (quoted in `KIT_UX_REDESIGN.md`):
they do not know what to do on each page, how they got there or why it is open; disclosure rows are
too small to hit; raw keys are meaningless. Persona and community research in
`docs/MODDER_JOURNEYS.md`: modders in comparable scenes extract, edit in a general tool (Blender,
image editor) and repack; a byte-exact round trip and standard formats earn trust; the fastest
edit-to-test loop wins.

**Context of use.** A Mac with a Retina display, keyboard and trackpad or mouse; the game running
at the same time in its own window; sessions of an hour or more; an image editor or Blender open
alongside; often no internet.

**Familiar tools, and what users carry over from each.** Users judge a new tool by the ones they
know (Jakob's law). The Kit borrows *conventions*, never looks:

| Tool | What its users expect | Where it lands in the Kit |
|---|---|---|
| Xcode | Sidebar · editor · inspector · bottom area; activity bar; Run/Stop with a target picker; live previews | Window anatomy, Test session controls, Jobs and Log |
| Reality Composer Pro | 3D view + scene outline + inspector | Track and car workspaces |
| System Settings | Grouped forms, always-open sections, plain labels | Handling, dealer, save forms |
| Photos / Finder | Sidebar + searchable thumbnail grid | Car and track pickers, textures |
| BeamNG.drive | Edit a file, press Ctrl+R in game, the vehicle reloads in place | The reload loop (6.4) |
| Assetto Corsa Content Manager | Car/track browser with real previews, showroom, one-click Drive | Pickers, showroom view, quick test |
| Unreal Editor | Content Browser, Details panel, Play-in-Editor, reset-to-default arrow | Browser, inspector, ⟲ revert on each value |
| Blender | Outliner, properties, orbit navigation, export round trip | Track outliner, 3D navigation, OBJ/USDZ export |
| Forza / Gran Turismo livery editors | Paint on the 3D car, share a design | Paint tab on the car model |
| Mod Organizer 2 / Vortex / Nexus | Mod list with on/off, load order, conflicts | Mods: enable, order, "changes the same file as…" |
| Steam Workshop | Cover image, title, version, one-click install | One-file mod with a cover and notes |
| OpenIV | Archive browser with read-only and edit modes | Expert Game Files area |
| Bethesda Creation Kit | "Plugin is one file", "test in game" | Concepts only; its interface is widely disliked |

### 3. Jobs and tasks

The ranked job stories and their feasibility are in `KIT_UX_REDESIGN.md` §2 (17 jobs). The six that
the interface is built around, with the task steps each needs:

| Job | Steps the user takes | Steps the Kit must remove |
|---|---|---|
| Tune a car | pick car → change weight/power/grip → drive it → adjust | knowing physics.val, Save & Build, restarting |
| Repaint a car | pick car → pick livery → drop an image → see it on the car → drive | knowing which CT.RES number, size and palette limits |
| Change a sky | pick track → drop a panorama or a tile → see it → drive | tile maths, the command line |
| Put the bus on sale in Career | Career → Special vehicles → Put on sale → buy it in game | editing four tables and art by hand |
| Edit my save | open the save → change money/garage → play | hex editing, checksums |
| Share a mod | Share → one file → friend double-clicks | folder layouts, manifests |

Every one ends in the game, so **testing in the game is a first-class part of every task**, not a
separate tool (6.4).

### 4. Mental model

The model the Kit presents, in the words users already use:

- **A mod is a document.** You open it, edit it, it saves itself, you can revert to an earlier
  version, you share it as one file. (Every Mac user knows this from Pages and Keynote; every
  modder knows "plugin = one file".)
- **A mod is made of changes to things.** Things are cars, tracks, Career, saves, text, sounds.
  Each change is listed, can be undone and reverted, and is marked where it lives.
- **The game is a live preview.** It runs beside the Kit; Reload shows your changes; it never
  needs a restart for an ordinary change.
- **The original game is never touched.** Your mod is a layer on top.

Words: *car* (never chassis or archive), *paint* or *livery*, *handling*, *track*, *sky*, *props*,
*walls*, *racing line*, *event*, *dealer*, *save*, *mod*, *test*, *reload*, *install*, *share*.
Raw file names appear only as small secondary text and in Game Files.

### 5. Information architecture

**Organisation.** By *thing the user thinks about*, not by file type: Home, Cars, Tracks, Career,
Saves, Text & Menus, Sounds; then My Mod; then Game Files (expert). Within a thing, by *job*
(a car's Paint, Handling, Model, Test).

**Labelling.** Nouns for places, verbs for actions, plain words from section 4; units on every
number; "not used by the game" stated, not hidden.

**Navigation.**
- Global: the sidebar, always visible, at most two levels (the open car appears under Cars).
- Local: a segmented job bar at the top of each workspace.
- History: Back/Forward like Safari and Xcode.
- Contextual: links from the inspector ("Open Bus", "Show in Game Files").
- Wayfinding answers on every screen: *where am I* (window title, sidebar highlight, selected job),
  *how did I get here* (breadcrumb, Back), *why is it open* (breadcrumb root when opened from Home
  or search), *what next* (one prominent action).

**Search.** One field at the top of the sidebar finds cars, tracks, events, files and settings;
⇧⌘O opens a quick-open panel (Xcode convention).

**Home is a hub, not a wizard.** Job tiles deep-link into workspaces; no questions are asked. NN/G:
wizards suit rare tasks and frustrate repeat users; modding is repeat work.

### 6. Interaction design

#### 6.1 Object, then action
Select a thing (a value, livery, sky tile, prop, dealer row); the inspector explains it; actions
apply to the selection (toolbar, context menu, keyboard). Same model as Finder and Xcode.

#### 6.2 Direct manipulation
Drag a slider and see the number; drag a racing-line point on the map; drop an image on a livery or
tile; drag a prop in the top-down view. Every direct edit is also possible by typing a number.

#### 6.3 Feedback, undo and safety
- Every edit appears instantly in the field, in the Changes list and as an orange dot on the field,
  the job tab, the sidebar row and the picker tile.
- Undo/redo with ⌘Z / ⇧⌘Z for every edit; ⟲ revert per value, per thing, per mod.
- Autosave; File › Revert To for earlier versions.
- Nothing that rewrites game data is irreversible inside the Kit; the original game files are
  never written.

#### 6.4 The live test session (no restarts)
**States:** *Not running* → *Starting* → *Running* (in menus / in an event) → *Reloading* →
*Running*; any state → *Stopped*; *Crashed* shows the reason and a Restart button.

**Controls** (toolbar, like Xcode's Run/Stop): ▶ **Play** with a picker for track, event, car and
paint; ⟳ **Reload** (⌘R); ↺ **Restart Event**; ■ **Stop**. A Session tab in the bottom panel shows
state, what was reloaded, a live thumbnail and the game log.

**What Reload does, by kind of change.** The game reads car and track archives every time an event
loads, and texture replacements are applied by the renderer on the Mac side, so:

| Change | Shown in the game | How |
|---|---|---|
| Replacement textures (cars, tracks, skies, menus) | immediately, mid-race | renderer swaps the image |
| Car handling, car model, liveries, track props, walls, racing lines, event tables | at the next event load: Reload restarts the current event in seconds | the virtual disc re-reads the changed files |
| Things the game reads once at start-up (expected: game text, some menu tables) | after Restart Game, done for you, back to the same event | boot, then direct start |

Which change falls in which row is measured per file type, not assumed; the Kit labels the Reload
button with what it will do ("Reload · restarts this event").

**Game side (runtime work, owned by the game runtime):** a local control channel per test profile;
re-reading the mod folder into the virtual disc with room for each file to grow in place; an
event restart that skips menus and memory-card screens; texture swap without a reload.

**Phase 1:** the game window sits beside the Kit. **Phase 2:** the game picture inside the Kit
window (shared GPU surface plus forwarded input, the way Xcode shows previews from another
process).

#### 6.5 Long work never blocks
Exports, builds, installs, 3D decoding, image decoding and game runs each run in their own lane
(AGENTS.md). The toolbar activity area shows the newest job; the Jobs tab lists all with progress
and Cancel. Only something being *written* is locked, and only against writes, with the reason
shown ("Read-only while the mod builds").

#### 6.6 Errors and empty states
Errors say what happened and what to do, at the place it happened. Empty states have one action
("Choose a car"). Unsupported capabilities are visible and disabled with a one-line reason.

### 7. Platform conventions: native SwiftUI, no home-made controls

Rule: use the system component; build a custom view only where no system component exists, and
say why. Native controls follow the current macOS design (Liquid Glass), accessibility, keyboard
navigation, dark and light mode, and future OS updates for free; home-made ones never do.

| Need | System component |
|---|---|
| Mod as a document; Open Recent; autosave; versions; double-click a `.eodmod` | `DocumentGroup` with a document type and exported `UTType` |
| Window: sidebar, content, inspector | `NavigationSplitView`, `.inspector` |
| Title and breadcrumb | `navigationTitle`, `navigationSubtitle` |
| Toolbar, Test session controls, activity | `.toolbar` with `ToolbarItem` placements; `Menu` for the target picker |
| Search and quick open | `.searchable`; a `.sheet` list for ⇧⌘O |
| Settings (game folder, game build) | `Settings` scene |
| Job bar | `Picker(.segmented)` |
| Value forms | `Form` with `.formStyle(.grouped)`, `Section` headers and footers, `LabeledContent`, `Slider` + `Stepper` + `TextField(value:format:)`, `Toggle`, `ColorPicker` |
| Data tables (dealer, strings, racing-line points, all values) | `Table` with sortable columns, selection, `contextMenu(forSelectionType:)` |
| Thumbnail grids | `LazyVGrid` with images decoded off the main thread |
| Drag and drop of images | `.dropDestination`, `Transferable` |
| Preview exports | `.quickLookPreview` |
| Menus and shortcuts | `CommandMenu`, `.keyboardShortcut`, `focusedValue`; Edit › Undo |
| Progress | `ProgressView`; `UserNotifications` when a long job ends in the background |
| Status icons and their motion | SF Symbols with `symbolEffect` |
| 3D car and track | `RealityView` (RealityKit), meshes from `MeshDescriptor`, textures from `TextureResource`, `.realityViewCameraControls(.orbit)`; SceneKit, used today, is deprecated |

**Custom, because nothing exists:** the top-down track map with walls, props and racing line over
the road outline (`Canvas`); the sky tile strip; the before/after texture comparison.

### 8. Layout and information design

**Window skeleton** (Xcode / Reality Composer Pro): sidebar (220 pt) · workspace · inspector
(240–380 pt) · one collapsible bottom panel (Changes, Issues, Jobs, Session, Log). Toolbar:
navigation and title on the left, activity in the centre, Changes, Test session and Install on the
right.

**Hierarchy.** Large title (the thing), one secondary line of facts (class, price, file), the job
bar, then content. Type size and weight carry hierarchy, not borders and boxes. Related values sit
together in grouped sections (proximity is the strongest grouping cue); sections stay open
(NN/G: accordions hide content people need, and their triangles are small targets).

**Values.** Plain label, raw name small beneath, control, number with monospaced digits, unit,
original value and ⟲ when changed. Values the game does not read are in their own section, shown
read-only with the reason.

**Targets.** At least 28 × 28 pt; segmented job bar at least 96 pt per segment; sidebar rows 28 pt.

### 9. Content design

Copy rules are in AGENTS.md and the copy table in `KIT_UX_REDESIGN.md` §5. In short: titles are
nouns, actions are verbs, reasons say what happens and what to do, no narration of visible
controls, no addresses or file-format jargon in the main view (evidence lives in the inspector's
"How we know").

### 10. Visual, motion and sound

**Identity: native window, the game in the content.** Apple's pro apps (Xcode, Final Cut, Logic)
keep standard chrome and let content carry identity. The Kit does the same:
- *Game identity*: app icon; accent colour taken from the game's menu art; a Home header with a car
  rendered from the player's own disc; real rendered thumbnails of every car and track; the game's
  fonts only inside game previews.
- *Not themed*: controls, scroll bars, window chrome, fonts in the interface. Light and dark follow
  the Mac setting (the current Kit forces dark; that goes).

**Colour meaning, used the same everywhere** (as in Xcode): orange dot = changed; green = done or
installed; red = problem; grey = not used by the game; accent = the one primary action per screen.

**Motion: brief, purposeful, interruptible, and off under Reduce Motion.**
- Picking a car zooms its tile into the workspace (`matchedGeometryEffect`), as Photos does.
- Numbers roll when a value changes (`.contentTransition(.numericText())`).
- Status icons pulse or bounce once (`symbolEffect`).
- Sheets slide from their source; sidebar and inspector slide.
- Nothing animates for decoration, and nobody waits for an animation to finish.
- `accessibilityReduceMotion` swaps movement for cross-fades.

**Sound.** The interface is silent, like Apple's pro apps. The one exception is the system
notification when a long job finishes while you are elsewhere. Game audio is content and is
expected: engine sounds with an rpm slider, effects, songs.

**3D.** Real models wherever a car or track appears: picker thumbnails, a turntable showroom view,
paint shown live on the car, the track as top-down map and 3D view. This is where the game's look
belongs.

### 11. Accessibility

Native controls give VoiceOver, full keyboard access and Dynamic Type-style scaling; custom views
(track map, tile strip, comparison, 3D views) get accessibility labels and keyboard selection.
Colour is never the only signal (the changed dot also has a tooltip and a Changes entry; status has
an icon). Contrast meets the HIG minimums in both modes. Reduce Motion is respected (section 10).

### 12. Evaluation

1. **Heuristic review** against NN/G's heuristics and ISO 9241-110 (done for 0.4.1/0.5.0 in
   `KIT_UX_REDESIGN.md` §1; repeat for each build).
2. **Flow sketch in HTML** (`docs/design/kit-prototype.html`): checks structure, navigation and the
   six job flows only. It is not the visual or interaction reference: an HTML page can only imitate
   native controls, Liquid Glass, RealityKit, keyboard focus and VoiceOver, which SwiftUI gives for
   free. It is not polished further.
3. **Prototype in SwiftUI from here on**, in the real app with Xcode previews, as a vertical slice:
   *Tune a car → Reload → drive it*, end to end in the real app and the real game, before widening
   to other jobs. Every later prototype is SwiftUI.
4. **Usability rounds of about five people** (NN/G: small, repeated tests find most problems):
   the owner plus four players or modders, each given the six job tasks with no instructions.
   Record time to success, wrong turns, and every "where is…?" question. Fix, then test again.
5. **Acceptance per job:** clicked through in the installed app, and the change seen in the game
   at maximum quality, with screenshots looked at (AGENTS.md real-game rule).
6. **In-product measures:** main-thread stall monitor (`EOD_KIT_MEASURE`), time from edit to the
   change visible in the game.

---

## Part C: decisions and build order

| Decision | Choice | Why |
|---|---|---|
| App archetype | Document-based app, Xcode-style window, live test session | Matches the mod-as-document and game-as-preview models (§4) |
| Entry | Jobs Home in front of an always-visible sidebar | Recognition over recall; no wizard (§5) |
| Values | Grouped forms, open sections, sliders with numbers and units | Fitts's law, NN/G on accordions, System Settings convention (§8) |
| Components | System SwiftUI only, three custom views | Platform conventions, free accessibility and OS updates (§7) |
| 3D | RealityKit `RealityView` | Current Apple 3D framework; SceneKit deprecated (§7) |
| Testing | Live session with Reload; no restart for ordinary changes | The fastest edit-to-test loop wins (BeamNG, Trackmania) (§6.4) |
| Identity | Native chrome, game in the content | Apple pro-app convention; legal limit on game art (§10) |
| Sound | Silent interface, game audio as content | Apple pro-app convention (§10) |

**Build order.**
1. Platform base: macOS 15+ target, `@Observable` models, document type, Settings scene, system
   light/dark.
2. Vertical slice: Cars → Handling form → live session with Reload (Kit side and game side).
3. Car Paint with drag-and-drop and the 3D car; Track Sky; Career dealer and "Put on sale";
   Save editor.
4. Track Props, Walls, Racing line on the top-down map with the road outline.
5. Mods: one-file share, double-click to open, enable and load order.
6. Usability round, fix, repeat; embedded game view (phase 2).

## Sources

- ISO 9241-210:2019, Human-centred design for interactive systems: https://www.iso.org/standard/77520.html
- ISO 9241-110:2020, Interaction principles (summary): https://userfocus.co.uk/resources/iso9241/part110.html
- Design Council, Framework for Innovation (Double Diamond): https://designcouncil.org.uk/our-resources/framework-for-innovation
- J. J. Garrett, The Elements of User Experience (overview): https://boxesandarrows.com/the-elements-of-user-experience/
- Goal-directed design, interview with Kim Goodwin: https://articles.centercentre.com/?p=135
- Rosenfeld, Morville & Arango, Information Architecture, 4th ed.: https://oreilly.com/library/view/information-architecture-4th/9781491913529
- Apple, Essential Design Principles (WWDC17 802): https://developer.apple.com/videos/play/wwdc2017/802/
- Apple, Principles of great design (WWDC26 250): https://developer.apple.com/videos/play/wwdc2026/250
- Apple HIG, Design principles: https://developer.apple.com/design/human-interface-guidelines/design-principles
- Apple HIG, Motion: https://developer.apple.com/design/human-interface-guidelines/motion
- NN/G, Gestalt proximity: https://www.nngroup.com/articles/gestalt-proximity/
- NN/G, Why you only need to test with 5 users: https://www.nngroup.com/articles/why-you-only-need-to-test-with-5-users/
- BeamNG wiki, Introduction to Vehicle Creation (reload in place): https://wiki.beamng.com/Introduction_to_Vehicle_Creation
- Further NN/G, HIG and tool sources: `docs/KIT_UX_REDESIGN.md` §7.2, `docs/MODDER_JOURNEYS.md` §5
