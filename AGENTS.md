# AGENTS

Read and follow the AI policy found in `docs/policies/AI.md`, and the coding policies found in `docs/policies/Coding.md`.

Do not, under any circumstances, ever submit PRs or push commits on behalf of the user.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Switch 2 (NX2) work

- Build: `ninja -C build -j$(nproc) yuzu eden-cli tests` (`bin/eglantine` is the Qt app, `bin/eden-cli` the SDL one used for unattended runs). Lowering tests: `./build/bin/tests "[ampere]"`.
- Skills under `.claude/skills/`: `nx2-shader-triage` (rejected native shaders, lowering, visual bugs) and `nx2-lab-automation` (booting a game hidden, scripted keys, frame capture with `tools/nx2/lab_ctl.py`).
- Bound every unattended emulator run (`lab_ctl.py launch --max-seconds`, then `lab_ctl.py stop`): a forgotten hidden instance holds the user's GPU.
- Live debug server (`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545`, adapter `tools/nx2/debug_mcp.py`): status, screenshots, log tail, frame captures, shader rejects, guest memory reads and input on a running game, without a relaunch. See `docs/nx2/debug-mcp.md`. Try a live experiment before asking the user for a new build or run, restore every hash you reject, and never start or stop an emulator the user launched.
