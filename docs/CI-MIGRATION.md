# CI migration checklist

This change replaces the monolithic `release-binaries.yml` workflow with an orchestrator, per-platform reusable workflows, version-management scripts, and rolling daily/branch release jobs.

## Files to add

```text
.github/
  actions/setup-build/action.yml
  workflows/
    ci.yml
    build-linux.yml
    build-windows.yml
    build-macos.yml
    release.yml
    bump-version.yml
    cleanup-branch.yml
VERSION
CHANGELOG.md
tools/ci/
  slugify.sh
  changelog-section.sh
  bump-version.sh
  prepare.sh
  release-notes.sh
docs/
  CI.md
  CI-MIGRATION.md
```

## Cutover steps

1. Review the proposed initial semantic version in `VERSION`. The included `0.1.0-rc1` is a suggested starting point; it is not inferred from existing `build-N` tags.
2. Add actual initial release notes under `## [0.1.0]` in `CHANGELOG.md`. The bundle intentionally leaves this section empty rather than inventing release notes; the version preflight refuses to publish until it contains real notes.
3. Confirm the current project still has the CMake targets and packaging inputs used by the reusable workflows: `yuzu`, `yuzu-cmd`, `eglantine.profdata`, `dist/Eglantine.png`, `dist/dev.eden_emu.eden.desktop`, Windows Vulkan installation script, and the platform dependency documentation.
4. Merge all files to `nx2` and the default branch. If those are different branches, keep all workflow and script files present on both; scheduled runs and workflow-dispatch discovery depend on the default branch copy.
5. In repository settings, allow GitHub Actions to create and approve pull requests. Add `DISCORD_WEBHOOK_URL` only if release notifications are desired. Optionally set `MACOS_RUNNER=macos-26` after validating its runtime compatibility.
6. Run `ci` manually against a non-protected test branch with `publish: false`, first one platform at a time, and inspect each uploaded artifact. Test all platforms before enabling normal publication.
7. Run `ci` on `nx2` with `kind: daily`, `publish: false`, and all platforms to verify the daily build scheme without creating a release. Then run with publishing enabled after review.
8. Verify a same-repository PR using the `ci:build` label. Verify a closed PR causes its branch release to be removed.
9. Once the new pipeline has produced a successful full set of artifacts and a release, delete or disable `.github/workflows/release-binaries.yml` so the old `build-<run number>` release path cannot publish in parallel.
10. Only then use `bump-version` for production release candidates and stable releases.

## Checks performed on the proposal

The files in this bundle incorporate corrections to the pasted draft:

- RC builds now set `NIGHTLY_BUILD=OFF`; only daily and branch builds use the nightly product name.
- `VERSION` parsing is strict, with escaped dots and no leading zeroes. A changelog-only push also triggers preflight so maintainers can fill in notes after an initial blocked run without changing the version again.
- Changelog section lookup is exact. Version bumps move the `Unreleased` body under the new version heading, leave a fresh `Unreleased` section, and date the heading on stable promotion.
- Branch release tags include a short hash of the original branch name to avoid slug collisions.
- Manual daily runs honour `publish: false` and are serialized with scheduled daily runs; manual daily runs are not cancelled as branch builds are.
- Scheme preparation and publishing both check out helper scripts from the trusted default branch. They fetch the target build commit only as Git data, preventing a PR from spoofing release outputs and ensuring the `contents: write` publisher never executes scripts from a PR/branch commit.
- macOS `find` grouping is shell-escaped correctly. The default runner is `macos-15` to preserve the current build environment; `macos-26` remains an explicit tested override, not the implicit default.
- The artifact download action is updated to the current v8 major; checkout/cache/upload actions are updated to current v7/v6/v7 majors respectively as of 2026-11-04.

## Items that only a real GitHub Actions run can verify

Static review cannot prove that Homebrew/MSYS2/Qt packages still resolve, that PGO flags remain compatible with the checked-in `eglantine.profdata`, that every packaging command succeeds on live runners, or that the generated macOS bundle runs on the oldest supported macOS release. Treat the first one-platform builds as migration tests, then run a complete matrix before retiring the old workflow.
