# OBS-Spectra

OBS-Spectra is a Windows fork of [OBS Studio](https://obsproject.com) for recording gameplay. It adds a loop recording that keeps running in the background, a clip editor, and the [Lucida](https://github.com/karlockhart/lucida) chat logger and the Obscura chat censoring tool, built in as native plugins. Everything OBS Studio does is still there.

It is aimed at FiveM / GTA V roleplay, but the loop recording and clip editor work with any game.

OBS-Spectra is an unofficial fork. It is **not made or endorsed by the OBS Project**, so please don't send OBS-Spectra problems to the OBS Studio bug tracker, forums or Discord.

## Features

What OBS-Spectra adds to OBS Studio, and where to find it. [Feature details](#feature-details) below says more about the larger ones.

### Recording and clips

- **Loop recording**: records in the background as segments within a disk quota (100 GB by default) and deletes the oldest footage to stay under it. Folders, quota and segment length are in *Spectra > Loop Recording Settings...*.
- **Overflow folder**: instead of deleting the oldest recording, the loop can move it to another folder, say on another drive, with a limit of its own (*move the oldest recording to another folder* in Loop Recording Settings).
- **Starts with the game**: armed, the loop recording starts by itself when a listed game runs (`FiveM*` by default, and optionally any fullscreen application) and can stop when it exits. Arm it with the loop recording button or *Spectra > Armed*.
- **Automatic game capture**: adds a game capture of the detected game to the scene, falling back to window capture, and can fit the game to the canvas (Loop Recording Settings).
- **Clip Last**: saves the last part of the loop as one file, from *Spectra > Clip Last* or the clip hotkey (`Ctrl+F10`, its length set in Loop Recording Settings).
- **Events**: the event hotkey (`Ctrl+F9`) marks the moment in the loop recording; five named events each have a hotkey of their own (names in Loop Recording Settings, keys in *Settings > Hotkeys*). They show as pins in the Clip Maker.
- **Loop Recordings** (*Spectra > Loop Recordings...*): the loop by day, session and segment, with sizes and events. Back Up, Archive or Delete blocks of time, ticked or picked with *From*/*to* or *Around* an event, as one lossless `.mkv` per block or the segments as they are. Events go along with the copies.
- **Separate speaker tracks**: next to the full mix, the loop can record you, TeamSpeak and the game on tracks of their own, for transcripts (Loop Recording Settings).
- **Write check**: if the loop recording stops reaching the disk, OBS-Spectra says so in the status bar, the tray and over the game.
- **Recording resolution and quality**: set in Loop Recording Settings and the setup wizard.

### Clip Maker

*Spectra > Clip Maker...*

- **Two modes**: make clips from the whole loop recording (or another folder of recordings, such as a backup), or trim a saved clip or any video file.
- **Timeline**: zoom, recording sessions, the date and time each moment was recorded, and keyboard controls (`I`/`O`, `J`/`L`, `Space`, `Home`/`End`).
- **Event pins**: point at one to see its event, click it to go there, double-click it to name or rename it. Pins from Lucida are violet, the ones you mark are amber.
- **Censor layers**: rectangles and ellipses over the video, filled with a solid color, pixelation or blur, each shown for a part of the timeline you choose.
- **Captions** (Captions tab): transcribe In to Out with Whisper, edit them, show who is speaking, then burn them into the video, save an `.srt` next to the clip, or both. Needs speech recognition.
- **Export**: Original (lossless copy), High, Medium, Small or YouTube 1080p, with *Frame-accurate* for a clip that starts exactly on the In point.
- **Upload to YouTube**: sign in with Google once, then export and upload with a title, description, tags and visibility.

### Screenshots

- **Screenshot hotkey** (`Ctrl+F11`): the game at its own resolution as a lossless PNG.
- **Screenshot gallery** (*Spectra > Screenshots...*): thumbnails and a preview of the screenshots folder. Open, show in folder, copy or delete (to the Recycle Bin), **Send to Obscura** to censor the chat, or **Upload as Is** to imgbb.

### Lucida chat log

- **Chat logging**: reads the game's chat from the game capture with OCR and saves each line once to a searchable log, shown in the Chat Log dock. Lines logged during loop recording are linked to that moment of the recording.
- **Carnivore mode** (Chat Log dock or Lucida Settings): reads all the text on screen, not just the chat box, and logs each block under the screen region it was in.
- **Game profiles and regions** (*Tools > Lucida Game Profiles & Regions*): settings per game executable; draw, name, merge or ignore regions on a screenshot and test them.
- **Tags** (Lucida Settings > Tags): rules of trigger words that tag lines, with wildcards, one-typo tolerance for long words, a line to try them on, and re-tagging of the whole log.
- **Events and auto clips** (Lucida Settings > Events): a line that matches a rule pins an event on the loop recording, and with *Auto clip* also saves a clip around it.
- **Log browser** (*Browse...* in the dock, *Tools > Lucida Chat Log*, or `lucida-viewer.exe` without starting OBS-Spectra): full-text search, filters by channel, tag, region and time, the screenshot each line came from, a gallery of kept screenshots, playing the loop recording from a line (in VLC or mpv), opening the moment in the Clip Maker, and censoring a screenshot to save or upload it.
- **Screenshots and sampling** (Lucida Settings): keep a screenshot of the game when new lines appear (PNG or JPEG), order lines by the HUD clock, sample faster when the chat is busy, and keep the log for a number of days or forever.
- **Speech transcription** (Lucida Settings > Speech): Whisper turns what is said in the loop recording into log lines, on this PC, on the graphics card if it supports Vulkan. Per speaker with speaker tracks, a language and a vocabulary of names and places, after each segment or after the game closes, and for older recordings too.
- **Sharing on the local network** (Lucida Settings > Network): pair with other OBS-Spectra PCs to search each other's chat logs and download each other's clips.
- **Storage PC** (Lucida Settings > Network): one paired PC keeps the others' loop recordings and chat logs, and can transcribe their speech for them.

### Obscura chat censoring

- **Capture hotkeys**: `Ctrl+F12` captures the game and checks its chat; `Ctrl+Shift+F12` freezes the screen to snip a region, censors it and uploads it to imgbb.
- **Review** (Obscura dock, *Tools > Obscura Review Window*): Obscura suggests which chat lines to hide and learns from what you choose; draw boxes of your own, or set where the chat is. It can save by itself once its suggestions are confident.
- **Watch folders**: censor new screenshots that appear in folders you pick.
- **imgbb upload** with your own API key (kept in Windows Credential Manager): link expiry, an album, the link copied or opened, every upload logged, and optional automatic upload.
- **Shared definitions**: signed definitions downloaded from Obscura's public release channel as a starting point.
- Shares its settings and learning data with the standalone Obscura app.

### Editions and feature switches

- **Full, Lite and Ultralight** downloads: Lite leaves out Lucida, Obscura and speech recognition; Ultralight also leaves out the browser and streaming (see [Download](#download)).
- **Spectra > Features...**: switch Lucida, Obscura, speech recognition, the browser and streaming off and on (those your edition has), one by one or with the *Lite* and *Ultralight* presets. A feature that's off isn't started, its settings and data are kept, and changes apply when OBS-Spectra restarts, which the dialog offers to do.

### Install and updates

- **Windows installer** for you alone (no administrator rights) or for everyone on the PC, with the Visual C++ runtime; or a zip, or a portable zip.
- **Settings** in `%USERPROFILE%\Spectra`, moved there once from `%APPDATA%\OBS-Spectra`, where earlier versions kept them.
- **Updates**: a daily check and *Help > Check For Updates*, a Stable or Release candidates channel (*Settings > General > Updates*), and **Update Now**, which runs the new installer or swaps in the new zip.
- **Code signed** release builds.
- **Based on OBS Studio 33.0.1**, and merges upstream releases.

### Other

- **In-game notifications** (Loop Recording Settings): small toasts in a corner of the game when loop recording starts or stops, a clip or screenshot is saved, or an upload finishes. They are a separate click-through window; nothing is injected into the game.
- **Audio Setup** (*Spectra > Audio Setup...*): the microphone with push-to-talk on keys, extra mouse buttons or gamepad buttons, a release delay, other applications' audio (such as voice chat), TeamSpeak capture (off by default) and desktop audio.
- **App picker**: pick the games and programs to capture or record from the running ones, or type a name.
- **Shortcuts** (Loop Recording Settings and the setup wizard): the clip, screenshot and Obscura hotkeys in one place.
- **Default scene sources**, and *Spectra > Reset Sources to Defaults...* to put them back.
- **Setup wizard page**: where Spectra keeps its recordings, clips, screenshots and data, and which game to watch for.
- **Splash screen** while OBS-Spectra starts.

## Download

Get the latest build from [Releases](https://github.com/karlockhart/obs-spectra/releases). Every release has a Windows x64 installer and two zips:

- `OBS-Spectra-<version>-Windows-x64-Setup.exe` installs OBS-Spectra for you alone (no administrator rights needed, in `%LOCALAPPDATA%\Programs\OBS-Spectra`) or for everyone on the PC (in `Program Files`), with Start menu shortcuts and an uninstaller. It installs the Microsoft Visual C++ runtime if the PC doesn't have it. Settings are kept in `%USERPROFILE%\Spectra`, and uninstalling leaves them there.
- `OBS-Spectra-<version>-Windows-x64.zip` keeps its settings in `%USERPROFILE%\Spectra` (earlier versions kept them in `%APPDATA%\OBS-Spectra`; they move them there on their first start). It never touches an existing OBS Studio install's settings.
- `OBS-Spectra-<version>-Windows-x64-Portable.zip` keeps its settings next to the program, so you can run it from any folder or a USB drive.

There are two smaller editions, each as the same three files with its name added (`...-Windows-x64-Lite-Setup.exe`, `...-Windows-x64-Lite.zip`, `...-Windows-x64-Lite-Portable.zip`, and likewise `-Ultralight`):

- **OBS-Spectra Lite** has everything in the [feature list](#features) except Lucida, Obscura and speech recognition (so the Clip Maker has no captions), and leaves out the OCR and speech libraries they need.
- **OBS-Spectra Ultralight** is Lite without the built-in browser (CEF) and without streaming: no browser sources, browser docks or stream settings, and nothing can start a stream. It is for recording only. The Clip Maker can still upload to YouTube, because it signs in through your own web browser.

Each copy updates to the next version of its own edition.

The full OBS-Spectra can also be made as light as either of them without installing another edition: **Spectra > Features...** switches Lucida, Obscura, speech recognition, the browser and streaming on and off one by one, or all at once with the *Lite* and *Ultralight* presets. A switched-off feature isn't started at all (no OCR, speech models, browser docks or stream controls), and its settings and data are kept for when it's switched on again. Changes apply when OBS-Spectra restarts, which the dialog offers to do. Browser sources already in a scene keep working with the browser off.

Release builds are code signed (Azure Artifact Signing), so Windows shows OBS-Spectra's publisher on the installer and the program.

Run the installer, or unzip either zip and run `bin\64bit\obs-spectra.exe`. OBS-Spectra checks this repository's releases once a day (and from **Help > Check For Updates**) and offers to update itself. **Settings > General > Updates** picks the channel: **Stable** offers only releases, **Release candidates** the newest release or release candidate, whichever is newer. A release candidate starts on the release-candidate channel.

For an installed copy, **Update Now** downloads the new installer, checks it against its `.sha256` and closes OBS-Spectra; the installer then updates the copy where it is (asking for administrator rights if it was installed for everyone) and starts OBS-Spectra again.

For a zip copy, **Update Now** downloads the zip that matches your copy (portable or not), checks it against its `.sha256`, unpacks it into a `<folder>.update` folder next to OBS-Spectra's, then closes OBS-Spectra. The small `spectra-updater.exe` waits for it (and Lucida Viewer) to exit, renames the old folder aside, moves the new version into its place and starts OBS-Spectra again. The folder keeps its `config` (profiles, scenes, plugin settings, logs and downloaded speech models), its portable mode, and any file or folder of your own in its root; `bin`, `core`, `data` and `obs-plugins` are the new version's, so a plugin you added there has to be added again. If anything goes wrong the old folder is renamed back and starts as before. What the updater did is in `<folder>.update.log` next to the folder. Lucida's chat log, screenshots and loop recordings are kept outside the folder, so they're untouched.

A zip copy can only update itself where it may create a folder next to its own, so not under `C:\Program Files`; use the installer there, or download the new zip from the release page. To update by hand, unzip the new version and move your old folder's `config` (and `portable_mode.txt` for a portable copy) into it. Copies older than the in-app updater update with the `Update-Portable.cmd` in their folder, once; from then on OBS-Spectra updates itself.

Versions are named `<OBS version>-spectra.<n>`. For example, `33.0.1-spectra.1` is the first OBS-Spectra release based on OBS Studio 33.0.1. Release candidates end in `-rc.<n>` and are published as pre-releases.

## Feature details

**Overflow folder.** With *move the oldest recording to another folder* in Loop Recording Settings, the loop folder stays within its quota and the oldest segments go on to the overflow folder, whose own limit deletes its oldest recording (or *No limit* keeps everything until its drive is full). If the overflow folder can't take them, because its drive is unplugged or full, they are deleted once the loop is a tenth past its quota, so recording never fills its drive.

**Loop Recordings** (*Spectra > Loop Recordings...*). The loop recording by day, session and segment, with what each takes on disk. Tick blocks of time, or pick one with *From* and *to*, then **Back Up** (copy them to a backup folder and keep them in the loop), **Archive** (copy them to an archive folder, say on another drive, then remove them from the loop) or **Delete** them. Copies are one lossless `.mkv` per block of time, or the segments as they are; *Trim to exactly this time* cuts a block to the minute you picked instead of whole segments.

**Events.** The event hotkey (`Ctrl+F9`) marks the moment in the loop recording; five named events (named in Loop Recording Settings, each with a hotkey in Settings > Hotkeys) mark it with their name. The Clip Maker shows them as pins on its timeline: point at a pinhead to see which event it is, click it to go there, double-click it to name (or rename) the event. Loop Recordings lists each segment's events, can select the time around one (*Around*, a few minutes either side), and backups and archives take their events along: a folder of segments gets its own `spectra-events.json`, a single file a `<file>.events.json` next to it, so the pins are there when the Clip Maker opens them.

**Clip Maker** (*Spectra > Clip Maker...*). An editor for the whole loop recording or for a saved clip:
- Mark the clip by dragging on the timeline's clip strip, or with In and Out points at the playhead (keys `I` and `O`). Drag the handles to trim it and the bar to move it.
- Click anywhere on the timeline to play from there, or drag to scrub. Ctrl+scroll (or the zoom slider) zooms in on the timeline; scroll or middle-drag to move along it.
- Draw rectangles or ellipses over the video to censor it, using a solid color, pixelation or blur.
- Export the clip as one file at a chosen quality. Original copies the video without re-encoding (lossless); High, Medium and Small re-encode it, at roughly 15, 8 and 4 Mbps for 1080p60; YouTube 1080p follows YouTube's upload recommendations (at most 1080p, 12 Mbps at 60 fps). With Original, tick Frame-accurate to re-encode at High so the clip starts exactly on the In point; without it, and without censor layers or captions, the clip is copied losslessly from the keyframe just before the In point.
- Upload the clip to YouTube: sign in with Google once, give the video a title, description, tags and visibility, and the clip is exported and uploaded with a link at the end. Release builds carry the Google API client this needs (the `YOUTUBE_CLIENTID` and `YOUTUBE_SECRET` build settings).

**Lucida events and auto clips** (Lucida Settings > Events). Rules of triggers and an event name: when a chat or speech line matches one, Spectra pins that event on the loop recording, and with *Auto clip* ticked saves a clip around the moment too (how long before and after is set there, and a rule waits a while before firing again). Lucida's pins are violet in the Clip Maker; the ones you mark with the event hotkeys are always amber. Triggers match as tag triggers do.

**Sharing on the local network.** Turn it on in Lucida Settings > Network. The Chat Log dock then lists the other OBS-Spectra PCs on the same network and what they're playing. You pair with one once: both screens show a six-digit code, and each person confirms that the codes match. After that, each of you can search the other's chat log with its screenshots (the Network checkbox in the log browser) and download clips from the other's clips folder, for as long as OBS-Spectra is running there. Nothing is copied in the background unless you choose a storage PC (below), and unpaired PCs only see the PC's name. Connections are encrypted end to end (X25519 and ChaCha20-Poly1305). OBS-Spectra uses UDP port 47650 to find other PCs and TCP port 47651 (or a free one) for everything else, so allow it through Windows Firewall (on public networks too if Windows calls your home network public).

If two PCs don't see each other (some routers, mesh and guest Wi-Fi in particular, drop broadcasts), use **Add by address...** in the Chat Log dock on either one; the dock shows each PC's addresses while it has found nobody. Once paired, PCs remember where the others are, look for them there, and tell each other where their other paired PCs have gone, so a PC whose address changes finds its way back by itself. Routers with "client isolation" on keep Wi-Fi devices from reaching each other at all; that has to be turned off in the router.

**A storage PC.** A paired PC can keep the others' loop recordings and chat logs (Lucida Settings > Network: *Be a storage PC*, with its own folder and quota). A PC that stores on it sends each finished loop segment, its chat lines and its screenshots as they come, resuming a send that was cut off, and keeps its own loop as a buffer, so its loop quota can be small. The storage PC keeps each PC's recordings in a folder of its own and deletes the oldest segments, whichever PC recorded them, to stay under its quota, as the loop recording does; each PC's chat log becomes a copy that the log browser opens (*Stored PCs...* in the dock). The storage PC can also transcribe the speech (Lucida Settings > Speech: *Leave it to the storage PC* on the PC that records, *Transcribe for paired PCs that store here* on the storage PC): the transcripts come back into the recording PC's log, and each segment is transcribed once and appears in the log once.

**Obscura.** The capture hotkey reads the game from Spectra's game capture, so it works in fullscreen. A region capture freezes every monitor while you drag a rectangle, and that crop is searched for chat, censored, saved and uploaded to imgbb; Obscura's settings choose whether you review a crop first (only when it has chat to censor, always, or never). Uploads happen only when you ask, unless automatic upload is on, and every upload's link and delete link are logged to `imgbb-uploads.jsonl` in the Obscura folder. Obscura shares its settings and learning data with the standalone Obscura app.

**Screenshot gallery** (*Spectra > Screenshots...*). The screenshots folder as thumbnails, newest first, with a preview. Open, copy or delete (to the Recycle Bin) screenshots, or select some and **Send to Obscura** to have their chat found and censored, then saved or uploaded. **Upload as Is** sends them to imgbb through Obscura without censoring, after asking. HDR (JPEG XR) screenshots are listed but can't be previewed, censored or uploaded.

**Default scene sources.** Spectra's defaults are the default microphone with push-to-talk on `N`, `M`, `O` and `P`, desktop audio off, and Spectra's game and window captures. TeamSpeak audio is one of them if you turn on *Capture TeamSpeak audio* in the audio setup (off by default). *Spectra > Reset Sources to Defaults...* reapplies them without changing your other sources and scenes.

## Building

Only Windows x64 is supported. You need Visual Studio 2026 with the C++ desktop workload and the CMake bundled with it (3.28 to 3.30). Configuring needs an internet connection, because CMake downloads the dependencies listed under [Attribution](#attribution).

```
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

For Lite or Ultralight, set `SPECTRA_EDITION` in a build folder of its own, e.g. `cmake --preset windows-x64 -B build_ultralight -DSPECTRA_EDITION=Ultralight`.

Pull requests build and package OBS-Spectra in the release configuration (`.github/workflows/pr-pull.yaml`), unless only documentation changed. Pushes to master don't build: the pull request already built the merge. Each build records the source tree it was made from (`spectra-build-source.txt`).

Pushing a version tag publishes a release (see `.github/workflows/push.yaml`). When the tag is on the merge of a pull request from this repository, and that pull request's last successful build is of exactly the tagged source, the release ships that build instead of building again; otherwise the tag builds. The release job runs on Windows: `.github/scripts/Package-Spectra-Windows.ps1` makes each edition's zip, portable zip and installer (Inno Setup, `frontend/spectra-installer/OBS-Spectra.iss`), and stamps the release in `bin/64bit/obs-spectra-release.txt`. A build from a pull request compiled in a development version, and OBS-Spectra reports the stamp instead when it is a release of the same OBS version.

Builds make the Full edition, which *Spectra > Features...* switches down to what Lite and Ultralight have. Lite and Ultralight are built too with the repository variable `SPECTRA_EDITIONS=all`, a pull request labelled `all editions`, or the `all` input of a manual run, and a release ships them only then. A Lite or Ultralight copy updates only from its own edition's zips, so it stays on the last release that had them, until it is replaced by Full.

Signing is on once the repository has the variables `AZURE_CLIENT_ID` and `AZURE_TENANT_ID`: an Entra app with a federated credential for this repository's `release` environment, which only tag pushes may use. GitHub's token names the repository by its owner and repository ids, so the credential's subject is `repo:karlockhart@182013/obs-spectra@1381966325:environment:release` (a second one with the plain `repo:karlockhart/obs-spectra:environment:release` does no harm) and the *Artifact Signing Certificate Profile Signer* role on the certificate profile. `SPECTRA_SIGNING_ENDPOINT`, `SPECTRA_SIGNING_ACCOUNT` and `SPECTRA_SIGNING_PROFILE` override the account `.github/scripts/Sign-Spectra.ps1` signs with. Without them the release is built unsigned. To sign a local build with your own Azure sign-in (`az login`): `.github/scripts/Sign-Spectra.ps1 -Path <folder>`.

A release signs each binary once: the copies the three editions share take the one signed copy, so a release with all three editions uses about 250 signatures of the Basic plan's 5,000 a month (Microsoft bills each one past that). `SPECTRA_SIGNING_MAX` caps the signatures one release may use (1000 by default). When a release can't be signed (the cap reached, the account refusing, or the service still failing after retries), it is released unsigned and its notes say which editions; with `SPECTRA_SIGNING_FALLBACK=fail` it stops instead.

## License

OBS-Spectra is distributed under the **GNU General Public License v2.0 or later**, the same license as OBS Studio. See [COPYING](COPYING). Third-party components keep their own licenses, listed below.

## Attribution

### OBS Studio

OBS-Spectra is built on **OBS Studio** by the [OBS Project](https://obsproject.com) and its contributors, and almost all of its code comes from them. Upstream code is © its respective authors ([AUTHORS](AUTHORS)) and licensed under GPL-2.0-or-later. OBS-Spectra regularly merges upstream releases from [obsproject/obs-studio](https://github.com/obsproject/obs-studio). "OBS" and "OBS Studio" are the OBS Project's; the names appear here only to say where this fork comes from.

Everything OBS Studio itself depends on (Qt, FFmpeg, x264, CEF, obs-deps and the rest) is used under that project's own terms. See the [upstream repository](https://github.com/obsproject/obs-studio) and [obs-deps](https://github.com/obsproject/obs-deps) for those licenses.

If you want to support OBS Studio itself, see [obsproject.com/contribute](https://obsproject.com/contribute).

### Projects ported into OBS-Spectra

- **[Lucida](https://github.com/karlockhart/lucida)** by Karl Lockhart. A Python chat logger for FiveM, ported to C++ as the `spectra-lucida` plugin and `lucida-viewer`. Its logs can be read by both, because the SQLite database format is unchanged.
- **Obscura** by Karl Lockhart. A Python tool that censors chat screenshots, ported to C++ as the `spectra-obscura` plugin plus the shared `spectra-vision` and `spectra-censor` libraries. Obscura's source repository is private. OBS-Spectra checks signed definitions from its public release channel, [obscura-dist](https://github.com/karlockhart/obscura-dist), against Obscura's public key.

### Third-party components

CMake downloads these while configuring (with `spectra_fetch` from `cmake/spectra/SpectraDeps.cmake`) and checks each download against a pinned SHA-256. None of them is stored in this repository.

Their license files come with every build, in `data/spectra-vision/licenses/` and `data/spectra-speech/licenses/`. That includes ONNX Runtime's third-party notices, the RapidOCR and PaddleOCR licenses for the OCR models, and whisper.cpp's license.

| Component | Version | License | Used for |
| --- | --- | --- | --- |
| [ONNX Runtime](https://onnxruntime.ai) by Microsoft, from its [NuGet package](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime/1.30.0) | 1.30.0 | MIT | Running the OCR models (`spectra-vision`). |
| [RapidOCR](https://github.com/RapidAI/RapidOCR) ONNX models by RapidAI: PP-OCRv6 small detection and recognition models | 3.9.2 | Apache-2.0 | Reading text on screen. The models are [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)'s (PaddlePaddle, Apache-2.0), converted to ONNX by RapidAI and downloaded from [ModelScope](https://www.modelscope.cn/models/RapidAI/RapidOCR). |
| [SQLite](https://www.sqlite.org) | 3.53.4 | Public domain | Lucida's chat log, with full-text search (FTS5). |
| [whisper.cpp](https://github.com/ggml-org/whisper.cpp) by the ggml authors, with [ggml](https://github.com/ggml-org/ggml) built in | 1.9.4 | MIT | Speech recognition (`spectra-speech`): Lucida's transcripts and the Clip Maker's captions, on the CPU or, with Vulkan, the graphics card. |
| [Monocypher](https://monocypher.org) by Loup Vaillant and contributors | 4.0.2 | CC0-1.0 or BSD-2-Clause | Checking the Ed25519 signatures on Obscura's definitions and encrypting Lucida's connections to paired PCs on the local network. |

### Models downloaded when you use them

OBS-Spectra doesn't ship these. It downloads the ones you pick in Lucida Settings > Speech (or the Clip Maker's Captions tab) from Hugging Face, pinned to a revision and checked against a SHA-256:

- **[Whisper](https://github.com/openai/whisper)** speech recognition models by OpenAI (MIT), in ggml's format from [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp).
- **[Silero VAD](https://github.com/snakers4/silero-vad)** v6.2.0 voice activity detection by Silero Team (MIT), in ggml's format from [ggml-org/whisper-vad](https://huggingface.co/ggml-org/whisper-vad).

### Algorithms reimplemented from other projects

These were rewritten in C++ from scratch; no source code was copied. They are listed here because the behavior, and in places the exact results, follow the original projects:

- **[RapidOCR](https://github.com/RapidAI/RapidOCR)** (Apache-2.0): the steps and default settings for finding and reading text.
- **[OpenCV](https://opencv.org)** (Apache-2.0): small copies of the resizing, image warping, color conversion, contour and rotated-rectangle functions, which match OpenCV's results exactly. OpenCV itself is not used. The same goes for [pyclipper](https://github.com/fonttools/pyclipper) / [Clipper](https://www.angusj.com/clipper2/) and [Shapely](https://github.com/shapely/shapely), which the Python originals used.
- **[CPython's `difflib`](https://github.com/python/cpython/blob/main/Lib/difflib.py)** (Python Software Foundation License): `SequenceMatcher.ratio` and `get_close_matches`, used to spot duplicate chat lines.
- **[scikit-learn](https://scikit-learn.org)** (BSD-3-Clause): `TfidfVectorizer` with character n-grams and `LogisticRegression` with balanced class weights and the L-BFGS solver, used for Obscura's learning.

### Services

- **[imgbb](https://imgbb.com)**: uploads censored screenshots with the user's own API key. The key is stored in Windows Credential Manager.
- **GitHub Releases API**: checks for new OBS-Spectra and OBS Studio releases and downloads Obscura definitions.

### Tooling

Much of OBS-Spectra's code was written with help from [Claude Code](https://claude.com/claude-code) (Anthropic); commits it co-wrote say so.
