# Shader catalogue

One file per game, `<program id>.json` (16 hex digits, upper case), naming the native programs
people have identified: what they draw or compute and how that was found. The shader lab
(`tools/nx2/shader_lab.py`) shows these names next to each program, and `tools/nx2/catalog.py`
looks programs up from the command line.

```json
{
 "title": "0400486024DEC000",
 "game": "The Legend of Zelda: Ocarina of Time",
 "notes": "frame structure and anything that helps read the entries",
 "programs": [
  {"key": "...", "hash": "...", "stage": "pixel", "object": "Tree canopy leaf cards",
   "group": "Foliage / trees", "how": "hidden live: every tree goes bare", "notes": "...",
   "source": "build and settings it was found with", "updated": "2026-11-06T17:36:00Z"}
 ]
}
```

- `key`: native program key. The shader dump file name (`<key>-gfx-<addr>.bin`), the shader lab
  and `ablate.txt` use it. It is a hash of the game's code, so it is the same in every build.
- `hash`: shader hash, as the frame dump (`"shaders"`), `reject_shaders` and
  `EDEN_NX2_REJECT_HASH` show it. It is a hash of the lowered code, so it changes when the lowering
  of that program changes. Entries copied from another build may have only a hash; the shader lab
  fills in the key when it sees a program with that hash.
- `stage`: vertex, mesh, task, geometry, pixel or compute. `how`: what showed it (a live reject and
  what vanished, frame dump bindings, disassembly). `updated`: UTC time of the last edit; merging
  keeps the newer text field by field.

Your local copy (`EDEN_NX2_CATALOG_DIR`, default `%APPDATA%\eglantine\nx2-catalog` or
`~/.local/share/eglantine/nx2-catalog`) collects what you name in the lab and carries over between
builds and checkouts. Before a commit:

```sh
python3 -I tools/nx2/catalog.py status   # exit 1: your copy has entries the repo copy lacks
python3 -I tools/nx2/catalog.py merge    # bring them in, then commit the JSON with your work
```

Keep entries to names, stages and short notes: no shader code, textures, dumps or other game data.
