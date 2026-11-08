# AGENTS

Read and follow the AI policy found in `docs/policies/AI.md`, and the coding policies found in `docs/policies/Coding.md`.

Do not, under any circumstances, ever submit PRs or push commits on behalf of the user.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Switch 2 (NX2) work

- Build: `ninja -C build -j$(nproc) yuzu eden-cli tests` (`bin/eglantine` is the Qt app, `bin/eden-cli` the SDL one used for unattended runs). Lowering tests: `./build/bin/tests "[ampere]"`.
- Skills under `.claude/skills/`: `nx2-shader-triage` (rejected native shaders, lowering, visual bugs) and `nx2-lab-automation` (booting a game hidden, scripted keys, frame capture with `tools/nx2/lab_ctl.py`).
- Bound every unattended emulator run (`lab_ctl.py launch --max-seconds`, then `lab_ctl.py stop`): a forgotten hidden instance holds the user's GPU.
- Live debug server (`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545`, adapter `tools/nx2/debug_mcp.py`): status, screenshots, log tail, frame captures, shader rejects, guest memory reads and input on a running game, without a relaunch. See `docs/nx2/debug-mcp.md`. Try a live experiment before asking the user for a new build or run, restore every hash you reject, and never start or stop an emulator the user launched.

## Shader catalogue

`tools/nx2/catalog/<program id>.json` lists, per game, the native programs people have identified (what they draw, how it was found), so the next investigation starts from them. Entries are keyed by native program key (the shader dump and shader lab key, the same in every build); `hash` is the frame dump / `reject_shaders` hash, which changes when that program's lowering changes. `tools/nx2/catalog/README.md` has the format.

- The shader lab (`tools/nx2/shader_lab.py`) saves what you name to your local copy (`EDEN_NX2_CATALOG_DIR`, default `%APPDATA%\eglantine\nx2-catalog` or `~/.local/share/eglantine/nx2-catalog`), which carries over between builds and checkouts. Programs identified another way (debug server rejects, frame dumps): `python3 -I tools/nx2/catalog.py add --title <program id> --key <key> [--hash <hash>] --object "..." --how "..."`.
- **Every commit uploads the catalogue when yours has more.** Before each commit run `python3 -I tools/nx2/catalog.py status`. If it exits 1 (your local copy has entries the repo copy lacks, or newer ones), run `python3 -I tools/nx2/catalog.py merge` and commit the changed `tools/nx2/catalog/*.json` as its own commit (`tools/nx2: catalogue: <game>: <n> programs`) next to your work, so it goes up with the same push. If the repo copy already has everything, change nothing. `merge` only adds and updates; never delete entries from the repo copy.
- Names, stages and short notes only: no shader code, textures, dumps or other game data.
