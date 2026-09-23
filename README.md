# OBS-Spectra

OBS-Spectra is a Windows fork of [OBS Studio](https://obsproject.com) for recording gameplay. It adds:

- a loop recording that keeps running in the background
- a clip editor
- the [Lucida](https://github.com/karlockhart/lucida) chat logger and the Obscura chat censoring tool, built in as native plugins

It is aimed at FiveM / GTA V roleplay, but the loop recording and clip editor work with any game.

OBS-Spectra is an unofficial fork. It is **not made or endorsed by the OBS Project**, so please don't send OBS-Spectra problems to the OBS Studio bug tracker, forums or Discord.

## Download

Get the latest build from [Releases](https://github.com/karlockhart/obs-spectra/releases). Every release has two Windows x64 zips:

- `OBS-Spectra-<version>-Windows-x64.zip` keeps its settings in `%APPDATA%\OBS-Spectra`. It never touches an existing OBS Studio install's settings.
- `OBS-Spectra-<version>-Windows-x64-Portable.zip` keeps its settings next to the program, so you can run it from any folder or a USB drive.

Unzip either one and run `bin\64bit\obs-spectra.exe`. OBS-Spectra checks this repository's releases and tells you when a new version is available. It does not install updates by itself.

Versions are named `<OBS version>-spectra.<n>`. For example, `32.2.2-spectra.1` is the first OBS-Spectra release based on OBS Studio 32.2.2. Release candidates end in `-rc.<n>` and are published as pre-releases.

## What OBS-Spectra adds

**Loop recording.** Records continuously into a folder with a disk limit (100 GB by default), deleting the oldest footage when the limit is reached. It can start by itself when a listed game is running (`FiveM*` by default). *Spectra → Clip Last* saves the last few minutes as a single file.

**Clip Maker** (*Spectra → Clip Maker...*). An editor for the whole loop recording or for a saved clip:
- Set the start and end of a clip with In and Out points (keys `I` and `O`).
- Draw rectangles or ellipses over the video to censor it, using a solid color, pixelation or blur.
- Export the clip as one file. Without censoring it's copied without re-encoding (lossless); tick Frame-accurate to re-encode so the clip starts exactly on the In point.

**Lucida.** Logs in-game chat in the background. It reads the chat from the game capture with OCR and saves each line once to a searchable log. You browse the log in a Chat Log dock, and lines can be tagged by rules and linked to the moment in the loop recording where they appeared. `lucida-viewer.exe` opens the log without starting OBS-Spectra.

**Obscura.** Censors chat screenshots:
- Take a screenshot or snip a region with a hotkey (`Ctrl+F12` / `Ctrl+Shift+F12`).
- Obscura suggests which chat lines to hide and learns from the lines you choose.
- Upload the result to imgbb with your own API key.

It shares its settings and learning data with the standalone Obscura app.

**Other additions:**
- An audio setup dialog with push-to-talk, including on gamepad buttons.
- An app picker for choosing which games and programs to capture.
- Settings for Spectra's keyboard shortcuts.
- Default scene sources, including TeamSpeak audio.
- A setup wizard page and the update check.

## Building

Only Windows x64 is supported. You need Visual Studio 2026 with the C++ desktop workload and the CMake bundled with it (3.28 to 3.30). Configuring needs an internet connection, because CMake downloads the dependencies listed under [Attribution](#attribution).

```
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

Pushing a version tag makes GitHub Actions build the release and publish it with both zips (see `.github/workflows/push.yaml`).

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

CMake downloads these while configuring (`cmake/spectra/SpectraDeps.cmake`) and checks each download against a pinned SHA-256. None of them is stored in this repository.

Their license files come with every build, in `data/spectra-vision/licenses/`. That includes ONNX Runtime's third-party notices, and the RapidOCR and PaddleOCR licenses for the models.

| Component | Version | License | Used for |
| --- | --- | --- | --- |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) by Microsoft | 1.30.0 | MIT | Running the OCR models (`spectra-vision`). |
| [RapidOCR](https://github.com/RapidAI/RapidOCR) ONNX models by RapidAI: PP-OCRv6 small detection and recognition models | 3.9.2 | Apache-2.0 | Reading text on screen. The models are [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)'s (PaddlePaddle, Apache-2.0), converted to ONNX by RapidAI and downloaded from [ModelScope](https://www.modelscope.cn/models/RapidAI/RapidOCR). |
| [SQLite](https://www.sqlite.org) | 3.53.4 | Public domain | Lucida's chat log, with full-text search (FTS5). |
| [Monocypher](https://monocypher.org) by Loup Vaillant and contributors | 4.0.2 | CC0-1.0 or BSD-2-Clause | Checking the Ed25519 signatures on Obscura's definitions. |

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
