# Windows Event wait verification

`event_wait_probe` links the production `Common::Event` implementation. It is
built on Windows x86-64 when `YUZU_TESTS=ON`:

```sh
cmake --build <build> --target event_wait_probe --parallel 12
<build>/bin/event_wait_probe.exe > waits-default.csv
<build>/bin/event_wait_probe.exe --maximum-resolution > waits-500us.csv
```

The second command requests the same maximum Windows timer resolution as the
emulator frontend. The request ends with the probe process. Neither command
starts the emulator or reads game data.

Each run performs 30 delayed-signal trials with a two-second timeout and a
requested five-millisecond signal delay. It then runs five unsignalled trials
at each timeout: 200 microseconds, 500 microseconds, one millisecond, ten
milliseconds, one second and two seconds. Exit status is nonzero for an early
unsignalled return, a missed signal or an unconsumed signal.

The first output line records the build, QPC and TSC frequencies, monitor
capabilities and Windows timer resolution. CSV durations are nanoseconds
computed from QPC ticks and its reported frequency. Signal timestamps are
relative to the start of `WaitFor`, not an assumed sleep duration. A signal
delay of `-1` means no setter thread exists. Preserve the raw CSVs when comparing
builds; these are wait-correctness measurements, not game frame-rate results.

The six original Event regression tests come from Eglantine
[#538](https://github.com/Djihads80/Eglantine/pull/538), commit `a51de161ed`.
That PR replaces the Windows waiting policy with kernel timers. This fix instead
retains MWAITX and retries nonsignalled wakes against a fixed deadline. Additional
tests cover deterministic deadline/budget arithmetic, reset-induced wakes and
extreme timeouts. Untimed `Event::Wait` remains the indefinite-wait API; negative
`WaitFor` durations are not an infinite-wait sentinel.
