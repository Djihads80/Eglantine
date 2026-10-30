# Known issues

| Issue | Details | Workaround |
| --- | --- | --- |
| Missing geometry, flickering objects | Some objects are drawn with task/mesh shaders, which Eden cannot run yet (NX2_NOTES rows 89, 97); a few other shader forms are rejected ("unsupported native opcode", "native callee overwrites link register") | None yet |
| Frame rate below 60 on heavy courses | The emulator is limited by one CPU thread (GPU command processing), see [`NX2_GPU_THREAD_SPLIT.md`](../../NX2_GPU_THREAD_SPLIT.md) | GPU accuracy Low; close other programs; AMD Fluid Motion Frames for a smoother picture |
| Short slowdowns the first time a course is played | New shaders are compiled by the driver | They disappear once the shader cache is filled |
| Occasional freeze on a still image | GPU macro desync in the split GPU thread ("Native MME ... parameter FIFO underflow" in the log) | Environment variable `EDEN_NX2_SPLIT=1` |
| Grand Prix not verified with this fork | Eglantine fixed the black cup-opening screen and its movie (NX2_NOTES rows 147-149); not retested here | Free Roam and single races |
| Log filled with `nx2:camera` messages | A GameChat camera command is not implemented and is answered with an automatic stub | Harmless |
| AMD: terrain arrays use a guessed sparse block shape | The driver reports no sparse block size; the emulator then uses Vulkan's standard one (log: "no block size from the driver") | Report terrain that looks wrong, with the log |
| Only tested on one machine | Ryzen 7 5800X + Radeon RX 6900 XT, Windows 11 | Reports from other hardware are welcome |
