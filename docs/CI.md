# Eglantine CI and release guide

This document describes the split GitHub Actions pipeline added with this change. It is not yet a live workflow until the files have been merged to the required branches and the repository settings below have been applied.

## What runs when

| Event                                                  | What it builds                                                        | Publishes?                                                                 |
| ------------------------------------------------------ | --------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| Push to `nx2` that changes `VERSION` or `CHANGELOG.md` | Stable release or release candidate, based on the suffix in `VERSION` | Yes, unless the exact tag already exists or the changelog section is empty |
| Scheduled run at 03:00 UTC daily                       | Latest `nx2` commit                                                   | Yes; skipped when the latest daily already points at that commit           |
| Actions → `ci` → Run workflow, `kind: branch`          | The selected branch                                                   | Yes by default; can be disabled                                            |
| Actions → `ci` → Run workflow, `kind: daily`           | `nx2` only, even if another branch is selected the run is rejected    | Yes by default; can be disabled                                            |
| Same-repository PR with label `ci:build`               | PR head commit; rebuilt on synchronize/reopen                         | Yes, as a rolling branch pre-release                                       |
| Fork PR                                                | No build from the base repository                                     | No; see “Forks” below                                                      |

**Important behavior change:** the previous `release-binaries.yml` built on selected source/build-file pushes to `nx2` and `nx2-experimental`. This design instead runs the release pipeline on `VERSION` or `CHANGELOG.md` changes, scheduled daily, manual requests, or explicitly labelled same-repository PRs. Ordinary source pushes do not trigger this release pipeline. A changelog-only push retries a previously blocked version release; once its tag exists, the duplicate check skips it. The `nx2-experimental` branch is not included in the new release triggers.

The schedule is defined in UTC, and GitHub schedules are not guaranteed to start at the exact minute. On public repositories, GitHub can disable scheduled workflows after 60 days without repository activity; high Actions load can also delay runs. The scheduled workflow runs from the repository's default branch, then checks out `nx2` for the actual build. See the one-time setup section: the reusable workflow files must also exist on the default branch.

**First-merge warning:** adding `ci.yml` and `VERSION` together to `nx2` triggers the initial version workflow. The supplied `CHANGELOG.md` intentionally leaves `[0.1.0]` empty rather than inventing release notes, so the first run fails safely at preflight until maintainers add real notes. Add the notes before the cutover merge to publish immediately, or add them in a follow-up changelog-only commit; `CHANGELOG.md` is also a push trigger for this recovery case. Confirm `VERSION`, permissions, and whether the first run should publish before merging the cutover.

## Version and changelog rules

`VERSION` is the single source of truth. It must contain exactly one version on one line:

```text
X.Y.Z
X.Y.Z-rcN
```

Examples: `0.1.0-rc1`, `0.1.0`, `0.1.1-rc1`. Numeric components cannot have leading zeroes. The bundle starts with `0.1.0-rc1` as a **suggested starting point**, not as an existing Eglantine version convention. The current public releases use `build-<run number>`; confirm the first semantic version with maintainers before merging this value. The included `[0.1.0]` changelog section is intentionally empty and must receive the real initial release notes before the first versioned release can publish.

Add changes to the top `## [Unreleased]` section in `CHANGELOG.md`. When a new candidate is prepared, the bump script moves those entries beneath `## [X.Y.Z]` and leaves an empty `## [Unreleased]` section ready for the next cycle. The RC and stable release notes come from that version section; an absent or empty section fails before the build is published.

### Creating a release

1. For the initial `0.1.0-rc1`, add the real candidate notes under `## [0.1.0]`; after the first stable release, add new notes under `## [Unreleased]` and commit them to `nx2`.
2. Open Actions → `bump-version` → Run workflow. Choose one of:
   - `patch`: `0.1.0` → `0.1.1-rc1`.
   - `minor`: `0.1.0` → `0.2.0-rc1`.
   - `major`: `0.1.0` → `1.0.0-rc1`.
   - `rc`: `0.2.0-rc1` → `0.2.0-rc2`.
   - `release`: `0.2.0-rcN` → `0.2.0`.
3. The workflow commits the changed `VERSION` and `CHANGELOG.md` to `release/vX.Y.Z[-rcN]` and opens (or updates) a PR into `nx2`.
4. Review and merge the PR. The merge push changes `VERSION`, which starts `ci`.
5. Test the RC binaries before selecting `release` to promote that candidate to stable.

The bump workflow does not merge the PR for you. The merge is the approval gate. Merge or close the current release PR before starting another bump; otherwise the next run starts again from the still-unmerged `nx2` version. The helper script refuses to start a new major/minor/patch candidate while the current `VERSION` is still an RC; finish or promote that RC first. If branch protection requires CI on release PRs, add `ci:build` after reviewing the generated PR so it gets a CI run.

The CI duplicate check refuses to publish a tag that already exists. It does **not** compare the new semantic version numerically with the latest release, so maintainers must keep version numbers moving forward. If a failed run did not create the release/tag, re-run the workflow after fixing the failure.

## Running a build manually

Open Actions → `ci` → Run workflow. Select the branch to build and choose:

- **kind**: `branch` for a test build, or `daily` for a forced daily build of `nx2` only.
- **platforms**: `all`, `linux`, `windows`, or `macos`. A single-platform run produces a partial release with only that platform's assets.
- **pgo**: `none` or `both`. For manual runs, `none` is the default. `both` adds PGO variants where supported (Linux x86_64/ARM64 and Windows MinGW); macOS has no PGO variant.
- **publish**: enabled by default. Disable it to keep the build as workflow artifacts without creating/updating a GitHub release.

For automatic `nx2` version releases and scheduled daily builds, the workflow requests both PGO variants by default. Label-triggered PR builds default to non-PGO to keep test builds quicker.

A manual `daily` run is restricted to the `nx2` ref. It forces a fresh daily build for that date; with `publish: true`, it replaces the existing `daily-YYYYMMDD` release for the same day.

## Pull request and branch builds

For a same-repository PR, a maintainer should add the `ci:build` label only after reviewing the source as trusted. A build runs when the label is added, when the PR is synchronized with new commits, and when it is reopened (as long as the label remains). It checks out the PR head commit rather than GitHub's synthetic merge commit. The release job needs `contents: write` to create the rolling branch release, so the label is an explicit trust/approval boundary; do not use it as an automatic label for untrusted contribution code. The optional Discord webhook is not passed to PR-triggered release calls. The branch release is named `branch-<slug>-<short-hash>` and is deleted when the PR closes or its branch is deleted. A weekly sweep removes branch releases whose source branches no longer exist.

The hash suffix makes the generated branch tag collision-resistant even when two branch names normalize to the same text. Repeated builds of the same branch update the same rolling pre-release; older branch assets are replaced rather than accumulated.

### Forks

The base repository does not build or publish fork PR code. This is intentional: the workflow would otherwise execute untrusted code in a repository context with access to repository-level publishing permissions. A contributor who needs a build can enable Actions in their fork and run `ci` manually there; the release, if published, belongs to that fork.

Both the scheme-preparation job and the publishing job check out CI helper scripts from the repository's default branch, not from the branch/PR commit being packaged. The preparation job fetches the target commit as Git data and reads `VERSION` from it only for a version-triggered push. The publishing job fetches the target commit for Git history and, for stable/RC notes, reads that commit's `CHANGELOG.md`. Do not change this separation: the release job has `contents: write` and must not execute scripts supplied by PR/branch build commits.

## Build outputs

| Platform            | Default output               | Optional PGO output    |
| ------------------- | ---------------------------- | ---------------------- |
| Linux x86_64        | AppImage and `.tar.gz`       | AppImage and `.tar.gz` |
| Linux aarch64       | AppImage and `.tar.gz`       | AppImage and `.tar.gz` |
| Windows x86_64      | MSVC `.zip`                  | MinGW/clang `.zip`     |
| macOS Apple Silicon | App bundle `.zip` and `.dmg` | Not built              |

The existing build steps use the repository's current CMake targets (`yuzu` and `yuzu-cmd`) and package the produced `eglantine` and `eden-cli` binaries. Linux AppImages require glibc 2.39 or newer. Linux tarballs use system Qt and other shared libraries. Windows archives contain the deployed runtime dependencies. macOS artifacts are ad-hoc signed, not notarized; Gatekeeper may require the user to approve the app on first launch.

Build artifacts are retained for seven days. Published GitHub Release assets remain available until a release is deleted. Daily releases are pruned to approximately the seven most recent; branch releases are rolling and are cleaned up when no longer needed.

## Release naming

| Kind              | Tag                    | Release title                            | GitHub Release flag                |
| ----------------- | ---------------------- | ---------------------------------------- | ---------------------------------- |
| Stable            | `vX.Y.Z`               | `Eglantine vX.Y.Z`                       | Not a pre-release; marked latest   |
| Release candidate | `vX.Y.Z-rcN`           | `Eglantine vX.Y.Z-rcN`                   | Pre-release                        |
| Daily             | `daily-YYYYMMDD`       | `Eglantine Build DD/MM/YYYY`             | Pre-release; replaces same-day tag |
| Branch / PR       | `branch-<slug>-<hash>` | `Eglantine Branch: <PR title or branch>` | Pre-release; rolling               |

Only stable and RC publications send the optional Discord notification. Daily and branch releases do not.

## How SCM version metadata reaches CMake

The local `setup-build` action writes the four root-level files consumed by the repository's `GetSCMRev` CMake module: `GIT-COMMIT`, `GIT-REFSPEC`, `GIT-TAG`, and (for stable/RC only) `GIT-RELEASE`. This avoids depending on tags being present in shallow checkouts or on a named branch being available in detached PR checkouts.

Stable and RC builds get `GIT-RELEASE`, so CMake marks them as release builds. Daily and branch builds omit that file and are configured with `NIGHTLY_BUILD=ON`; stable and RC builds are configured with it off. The project currently disables the update checker in the CI build flags. The SCM/update-server template still contains upstream Eden URLs; do not enable the updater until those URLs are intentionally migrated to Eglantine's own service.

## Troubleshooting

- **`ci` or `bump-version` is missing from Actions:** confirm its workflow file is on the repository's default branch.
- **Scheduled builds never appear:** confirm `ci.yml` is on the default branch, scheduled Actions are enabled, and the run has not been disabled after 60 days of public-repository inactivity. Schedule times can also be delayed by Actions load.
- **The version push is skipped:** check the `VERSION` syntax and whether `vX.Y.Z[-rcN]` already exists as a tag/release. A duplicate is skipped, not rebuilt.
- **Release notes step fails:** ensure the matching `## [X.Y.Z]` section in `CHANGELOG.md` contains at least one non-blank entry.
- **Bump workflow cannot create a PR:** enable “Allow GitHub Actions to create and approve pull requests”, or create the release PR manually.
- **Mac app fails on an older OS:** check the effective minimum OS of Homebrew dylibs. The project's CMake deployment target is 15.0, but dependencies built on a newer runner can raise the effective runtime requirement.
- **A branch pre-release remains after cleanup:** run Actions → `cleanup-branch` → Run workflow to sweep orphaned branch releases.
