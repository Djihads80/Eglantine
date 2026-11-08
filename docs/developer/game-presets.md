# Recommended game settings

`src/common/game_presets.h` contains the `Settings::game_presets` catalogue, keyed
by program ID. Each entry supplies a display name and the settings to recommend:

```cpp
{0x0100000000000000ULL,
 {"Example Game",
  {{"backend", RendererBackend::Vulkan},
   {"resolution_setup", ResolutionSetup::Res1X},
   {"use_asynchronous_shaders", true}}}},
```

Use the registered labels from `src/common/settings.h` as keys. Values accept
enums, booleans, and numbers. Only list the settings the game needs; unlisted
settings retain the user's preferences. Recommendations must use switchable
settings so they can be applied without changing global preferences.

Adding a catalogue entry is enough to enable the existing prompt, translated
setting descriptions, application, and saving behavior. No game-specific frontend
function, session flag, or configuration key is needed. Run
`./build/bin/tests "[game_presets]"` to check the catalogue's keys and values.

The Qt frontend asks once per program ID per session. Yes applies the preset for
that launch; Remember also saves it in the per-game configuration. Skip persists
the program ID in the UI configuration. Later launches then use the saved settings,
including manual edits. Command-line launches always use saved settings without
the recommendation prompt.

Existing MKW and OOT skip preferences migrate to the program-ID list on first load.
