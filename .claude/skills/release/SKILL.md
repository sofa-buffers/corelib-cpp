---
name: release
description: Cut a release of sofa-buffers-corelib-cpp — bump every version manifest, land it via a release PR, then tag and publish the GitHub Release. Use when the user asks to release / tag / publish a new version (e.g. "/release 0.11.0").
argument-hint: "[X.Y.Z]"
disable-model-invocation: true
---

# Release sofa-buffers-corelib-cpp

The **git tag `vX.Y.Z` is the single source of truth** for the version (CORELIB_PLAN §12.3).
Every manifest that carries a version must equal the tag *at the tagged commit*, or the
`Version consistency` workflow (`.github/workflows/version-consistency.yaml`, runs on
`v*` tag pushes only) fails the tag. Between releases a manifest may run ahead of the
newest tag — that is correct, not broken.

The flow mirrors v0.10.0: a `release/vX.Y.Z` branch with one `chore(release): X.Y.Z`
commit → PR → merge → GitHub Release that creates the tag on the merge commit.

Target version: `$ARGUMENTS` (if empty, propose one in step 2 and ask).

## 1. Preconditions — stop and report if any fails

```bash
git checkout main && git pull -p
git status --porcelain                        # must be empty
git describe --tags --abbrev=0                # newest tag, e.g. v0.10.0
git log --oneline $(git describe --tags --abbrev=0)..origin/main   # what goes into the release
gh run list --branch main --workflow ci.yml --limit 3   # newest run on main must be green
```

Nothing new since the last tag → there is nothing to release; say so.

## 2. Choose the version

Pre-1.0 rule of the SofaBuffers family: **a minor bump (0.x → 0.x+1) may break API or wire
output; a patch bump (0.x.y → 0.x.y+1) must not.** Read the commits since the last tag
(`feat!`/breaking notes, changed public API in `include/sofab/`, changed wire output, changed
shared test vectors) and propose minor vs. patch with the reason. The family releases are
aligned (v0.10.0 "aligns this library with the rest of the SofaBuffers family"), so ask the
user whether the version is dictated by a family-wide release before inventing one.
The new version must be greater than the newest tag and the tag must not exist yet
(`git ls-remote --tags origin vX.Y.Z` is empty).

## 3. Bump the manifests — exactly these two

| File | Line |
|---|---|
| `CMakeLists.txt` | `project(sofabuffers_cpp` → `    VERSION X.Y.Z` (feeds `PROJECT_VERSION`, the installed `…-config-version.cmake` with `SameMajorVersion`, and Doxygen's `PROJECT_NUMBER`) |
| `conanfile.py` | `version = "X.Y.Z"` |

**Do not touch** (they are not the package version):
- `include/sofab/sofab.hpp` `API_VERSION = 1` — the SofaBuffers public-API version (CORELIB_PLAN §6.2); only changes with the spec.
- `assets/test_vectors.json` `"version": 1` — the shared vector-file format, vendored from corelib-c-cpp.
- `Doxyfile.in` — takes `@PROJECT_VERSION@` automatically.

If a new versioned manifest was added since this skill was written (grep the old version:
`rg -n --hidden -g '!.git' -F 'OLD.VER.SION'`), bump it too and make sure
`version-consistency.yaml` checks it.

Verify locally with the same extraction the workflow uses:

```bash
V=X.Y.Z
[[ $(grep -oP '^\s*version\s*=\s*"\K[^"]+' conanfile.py | head -1) == "$V" ]] && echo conan ok
[[ $(grep -oP '^\s*VERSION \K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1) == "$V" ]] && echo cmake ok
```

## 4. Build and test

Cap parallelism at `nproc` (6 cores here — never more):

```bash
cmake -S . -B build/release-check -DCMAKE_BUILD_TYPE=Release
cmake --build build/release-check --parallel "$(nproc)"
ctest --test-dir build/release-check --output-on-failure
cmake --install build/release-check --prefix build/release-check/stage   # the config-version file must say X.Y.Z
find build/release-check/stage -name '*config-version.cmake' -exec grep -n 'PACKAGE_VERSION "' {} +
```

The version bump cannot change behaviour, so the CI matrix on the PR is the real gate.

## 5. Release PR

```bash
git checkout -b release/vX.Y.Z
git add CMakeLists.txt conanfile.py
git commit     # subject: chore(release): X.Y.Z
git push -u origin release/vX.Y.Z
gh pr create --base main --title "chore(release): X.Y.Z" --body ...
```

Commit body / PR body (style of v0.10.0): "Version bump only: every package manifest brought
in line with the `vX.Y.Z` tag that follows this merge. The git tag stays the source of
truth." — then what changed since the previous tag, **naming every breaking change** with
its Crucible finding / CORELIB_PLAN § reference, and why that makes it minor vs. patch.

Wait for CI to be green, then merge (ask the user first; try `gh pr merge` once —
if refused, hand the user the command).

## 6. Tag + GitHub Release

Only after the merge, on the merge commit on `main`:

```bash
git checkout main && git pull -p
gh release create vX.Y.Z --target "$(git rev-parse HEAD)" --title vX.Y.Z --notes-file <notes.md>
```

This is outward-facing and not reversible in practice — **confirm with the user before
running it.** Release notes: same content as the PR body, addressed to users (breaking
changes first). Not a pre-release unless the user says so.

## 7. After the tag

```bash
gh run list --workflow version-consistency.yaml --limit 1   # must be green for the new tag
gh run watch <run-id>
```

If it fails, a manifest disagrees with the tag: do **not** move the tag silently — report
to the user (fix = new patch release, or delete + re-create the tag only with explicit OK).

Finally delete the release branch locally (`git branch -D release/vX.Y.Z` — ask, it is a
squash/merge-deleted branch) and report: version, PR, release URL, consistency-check result.
Never touch `origin/badges`.
