# Windows WASAPI switching

Use this page for durable Windows WASAPI output-switching principles. Current
scope, validation notes, and next-step priorities belong in
`docs/bug/wasapi-anomaly-status.md`.

## Transaction boundaries

- Clearly separate active output-switch transactions from error-recovery
  transactions.
- Active output-switch transactions include user or automation triggered output
  mode changes, spatial-audio changes, output device changes, and forced output
  configuration refreshes.
- Error-recovery transactions include WASAPI errors, device invalidation, and
  output resource failures.
- Do not model active output switching as a special case of recovery, and do not
  mix recovery logic into active switching transactions.
- Route output mode, spatial-audio, device, and forced refresh changes through a
  unified high-level transaction entry in `WindowsWasapiAudioPlayer`.

## State and lifecycle

- Avoid growing implicit state machines through new boolean flags, debounce
  reasons, or transition markers. Prefer explicit enums, one transaction state
  object, or a small set of clear phases.
- Keep buffer, session, and generation lifecycle boundaries explicit.
- Old buffer data or old sessions must not cross into a new output mode,
  spatial-audio state, device, or format unless that reuse is clearly safe.
- Use hot reconfigure only when compatibility, recovery state,
  buffer/session/generation boundaries, and stale-data safety are all clear. If
  safety is unclear, prefer conservative rebuild.

## No masking unclear structure

- Do not use parameter tuning such as longer silence, warmup discard, fade
  duration, debounce, or local short-circuits to mask unclear switching
  structure.
- Do not hide unclear switching structure with timing hacks, debounce hacks,
  longer fades, or silence padding.
- Do not reintroduce render-continuity patches, startup-profile special cases, or
  extra recovery-window logic unless the transaction boundary is clear and the
  evidence supports it.
