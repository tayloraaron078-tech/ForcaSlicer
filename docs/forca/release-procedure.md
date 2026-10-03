# Publishing a Forca release

Forca is developed in a private repository. The public repository
([tayloraaron078-tech/ForcaSlicer](https://github.com/tayloraaron078-tech/ForcaSlicer)) receives **one commit per
release** whose content is exactly the validated development state. Its history is OrcaSlicer's history plus one
"Forca Slicer <version>" commit per release, authored by A.T. Creations with a GitHub no-reply address. When a
release includes a newer OrcaSlicer merge, the release commit also has that OrcaSlicer commit as a second parent, so
the public history keeps Orca's history.

`tools/forca/publish_release.ps1` does the mechanical part in four stages; each stage that changes something public
is its own command. Every push and every public action needs the maintainer's explicit OK.

## 1. Before you start (development repo)

- [ ] The release state is on `Multi_Interface`, committed and pushed to the private origin; the working tree is clean
      (the local-only drafts in `docs/forca/release/` may stay untracked).
- [ ] **Version:** `FORCA_VERSION` in `version.inc` is the new version (for example `0.1.0-alpha.2`). The update
      check compares it with the release tags, so it must go up with every release.
- [ ] **Changelog:** `CHANGELOG.md` has a `## <version> — <date>` section with everything since the last release, and
      an empty `## Unreleased` above it.
- [ ] **Validated:** a full build with tests (`build_win.bat -s --config relwithdebinfo --tests`), all test suites
      green, `tools/forca/check_identity.ps1` passes, and the hands-on checks in
      [`upstream-merge.md` §8](upstream-merge.md#8-validate) that the release's changes touch. `safe/known-good`
      points at this state.
- [ ] **Release notes:** `docs/forca/release/RELEASE_NOTES_<version>.md` (local only — it becomes the text of the
      GitHub release). First line `# Forca Slicer <version> …` (dropped on the release page, which has its own
      title); placeholders `<DATE>`, `<SHA256_SETUP>`, `<SHA256_ZIP>` are filled in by the script. Start from the
      previous release's notes; the download table must name `Forca_Slicer_<version>_win64_setup.exe` and
      `Forca_Slicer_<version>_win64_portable.zip`.
- [ ] `docs/forca/release/KNOWN_ISSUES.md` is current.

## 2. Build the release commit (local)

```powershell
powershell -ExecutionPolicy Bypass -File tools/forca/publish_release.ps1 -Prepare
```

Runs the identity checks, then creates branch `public/candidate`: tree = `Multi_Interface`, parent = the current
public `main` (+ the OrcaSlicer commit if this release merged a newer one). It verifies the tree and the author and
prints what changes against the public `main`. Nothing leaves the computer. Review the summary.

## 3. Publish the code (public)

```powershell
powershell -ExecutionPolicy Bypass -File tools/forca/publish_release.ps1 -Push
```

Pushes `public/candidate` to the public `main`, only as a fast-forward of the `main` that `-Prepare` built on.

## 4. Build the release files (local)

```powershell
powershell -ExecutionPolicy Bypass -File tools/forca/publish_release.ps1 -Build
```

Clones the public `main` into `%USERPROFILE%\Documents\GitHub\ForcaRelease` (must not exist; delete it after a
release) and builds the Release zip and installer there, so the app embeds the **public** commit hash (the
Troubleshoot Center links to it). It reuses the kept Release dependencies in
`%USERPROFILE%\Documents\GitHub\ForcaCleanBuildTest\deps\build` when `deps/` is unchanged since they were built
(`forca_deps_source.txt` in that folder records the `deps/` tree they came from); otherwise it stops — rerun with
`-RebuildDeps` to rebuild them into that folder first. Times seen: about 30 minutes with the kept dependencies, about
an hour with `-RebuildDeps`. Needs NSIS for the installer (CPack finds it even when it isn't on PATH).

It stages the upload set in `ForcaRelease\release_upload`: the two renamed files, `SHA256SUMS.txt` and
`release_notes.md`.

**Smoke test** the installer and the zip (install with the desktop icon, Help → About shows the version, slice
something; start the portable zip once).

## 5. Publish the release (public)

```powershell
powershell -ExecutionPolicy Bypass -File tools/forca/publish_release.ps1 -Release
```

Creates the GitHub release `v<version>` on the public commit — a **pre-release** unless you add `-Stable` — with the
three files and the notes, then checks that GitHub's SHA-256 digests match ours. Forca's update check lists the new
release right away.

## 6. Afterwards

- Private archive: tag the released development commit (`git tag -a v<version> …`) and push the tag; advance
  `safe/known-good` if it isn't there yet.
- Announce it (a Discussions post in Announcements; credit the testers whose reports shaped the release).
- Delete `ForcaRelease` (keep the dependency folder for the next release).

## Notes

- The public repository is never a remote of the development repository; the script uses its URL directly.
- If `-Push` says the public `main` moved (someone merged something there), run `-Prepare` again; the new
  candidate builds on top of it.
- The first release (`0.1.0-alpha.1`, 2026-09-26) was made by hand with the same steps; the script reproduces them.
