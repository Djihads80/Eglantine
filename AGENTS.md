# AGENTS

Read and follow the AI policy found in `docs/policies/AI.md`, and the coding policies found in `docs/policies/Coding.md`.

Do not, under any circumstances, ever submit PRs or push commits on behalf of the user.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Multi-Agent & Git Worktree Workflow

Multiple agents may work on different bugs and features concurrently. Follow these rules strictly to prevent branch cross-contamination:

1. **Keep the Main Workspace on `local-combined`:**
   - The main repository directory (`e:\Projects\Eglantine`) hosts the shared `build/` directory and must remain checked out on `local-combined` for local integration builds and testing.
   - Do **not** run `git checkout` to switch branches in `e:\Projects\Eglantine` while working on a feature or bugfix.

2. **Use a Dedicated `git worktree` per Feature/Bugfix:**
   - Every feature or bugfix branch must be branched directly off `upstream/nx2` (never stacked on top of another feature branch or `local-combined`).
   - Create or check out a dedicated sibling worktree for the task:
     ```bash
     # New feature/bugfix branch off upstream/nx2:
     git worktree add -b fix/<topic-name> ../Eglantine-<short-topic> upstream/nx2

     # Existing feature/bugfix branch:
     git worktree add ../Eglantine-<short-topic> fix/<topic-name>
     ```
   - Perform all code edits and commits for that task inside its worktree (`e:\Projects\Eglantine-<short-topic>`).
   - Verify that only the intended commit(s) are on the branch against `upstream/nx2` (`git log --oneline upstream/nx2..fix/<topic-name>`), and never commit local `AGENTS.md` changes to PR branches.

3. **Integrating into `local-combined` & Building:**
   - After committing changes in the topic worktree, merge the topic branch into `local-combined` in `e:\Projects\Eglantine`:
     ```bash
     git merge --no-edit fix/<topic-name>
     ```
   - Run builds from `e:\Projects\Eglantine` against the shared `build/` folder:
     ```powershell
     . .\tools\windows\load-msvc-env.ps1; cmake --build build
     ```
   - If resetting `local-combined` to `upstream/nx2` to re-merge active topic branches, preserve the local `AGENTS.md` modifications on `local-combined`.
