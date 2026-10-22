# Tuna Tuner release notes

## v0.5.0

Tuna Tuner is a monophonic vocal pitch-correction plug-in for one lead line. It detects pitch with WORLD-based analysis and shifts it toward the notes of the selected key and scale.

- macOS Universal: AU and VST3, arm64 + x86_64.
- Windows x64: VST3, built and tested by the native Windows release workflow;
  requires the Microsoft Visual C++ x64 Redistributable.
- macOS bundles are ad-hoc signed, not notarized. Windows builds are unsigned.
- Checks run for this release source on macOS: CTest 9/9 passed on a lightly loaded run (the pacing, golden-vector and formant sections of `waveform_renderer_test` skipped: reference audio absent, machine busy). `realtime_engine_test` reported worker underruns in most runs while other processes kept most cores busy. Editor snapshot rendered; AU and VST3 contain arm64 and x86_64 slices; ad-hoc signatures verified; package integrity checked.

Known limits: fixed 320 ms latency; monophonic, no harmonies; no formant correction; pitch shifting can move audio within a phrase by up to about 80 ms, and large sustained shifts can stutter at forced seams. Amount, Expression and Transition remain in saved state but are ignored. Under heavy CPU load the real-time worker can fall behind and drop audio; timing has not been qualified in a DAW.

Each ZIP includes installation instructions, dependency notices and build provenance, with a
SHA-256 checksum companion. No DAW-host test has been run.
