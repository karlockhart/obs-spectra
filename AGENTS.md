# Working on OBS-Spectra

Guidance for AI coding agents and anyone new to the repository. OBS-Spectra is a
fork of OBS Studio; upstream's layout applies. Spectra's own additions live in
`frontend/` (dialogs, utilities and widgets prefixed `Spectra` or `Clip`), in
`plugins/spectra-*`, and are described for users in `README.md`.

## Branches

- `master` is the public product. Every release is built from it (or a version
  tag on it) by CI, with the Google API client for the YouTube upload coming from
  repository secrets.
- **`feature/whisper` is a private build and is never merged to master.** It
  carries the Whisper speech work: the `plugins/spectra-speech` plugin, speech
  transcription into the Lucida log, and captioning in the Clip Maker
  (`frontend/utility/ClipCaptions.*` and the Captions tab of
  `SpectraClipMaker`). Everything else that lands on that branch is expected to
  reach master separately.
- Work meant for master goes on a branch off `master` and lands through a pull
  request; `feature/whisper` then merges master back in to stay current.
- When a change is made on `feature/whisper` but belongs on master, cherry-pick
  it onto a branch off master and strip the Whisper-specific parts (caption
  code, `ClipCaptions`, `spectra-speech`). Don't let those parts reach master.

## Building on Windows

The tree is configured in `build_x64` with Visual Studio; cmake and clang-format
come with Visual Studio and are not on PATH. Build only the frontend with:

```
cmake --build build_x64 --target obs-studio --config RelWithDebInfo
```

Format changed C++ with the repository's `.clang-format` before committing.
Sources are LF.

## YouTube upload

The Clip Maker's Upload to YouTube needs a Google OAuth "Desktop app" client.
Locally the build reads it from a `client_secret_*.json` in the repository root
or the build folder (both git-ignored); CI gets it from the `YOUTUBE_CLIENTID`
and `YOUTUBE_SECRET` secrets (see `frontend/cmake/feature-youtube.cmake`). Never
commit, print or paste the client file or its secret.

## Commit messages

Prefix with the area (`frontend:`, `spectra-lucida:`, `CI:`), write a sentence in
the imperative, and explain the why in the body.
