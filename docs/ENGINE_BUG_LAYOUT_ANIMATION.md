# rccore bug: layout animation tweens GONE components (2026-11-09)

**Fixed** in `../kobento-libs` (branch `fix/layout-anim-gone`, with `tests/layout_anim_test.cpp`);
the engine moved there from `remotecompose-experiments`, whose line numbers are the same.
Found on a CrowPanel 1.28" knob document (`knob_fx.rc`, no longer in this repository), reproduced on the Mac with the same engine.

## Symptoms

1. A component that becomes VISIBLE slides in from an old position instead of fading in place.
2. A component that is GONE and stays GONE (the remote's red "mic off" icon and "MUTED" text,
   `link.mic_muted` 0 throughout) is drawn for several frames while its column re-lays out.

## Reproduce

```sh
esp32/host/build/rc2png documents/knob/knob_fx.rc out.png --fit 240 240 \
    --named link.mic_gain=40 --named link.mic_muted=0 --named fx.page=0 --named fx.count=4 \
    --named fx.label=VISUAL "--named" "fx.text=VU meter" \
    --live fx.page=1 --live fx.page=0 --live-ms 33 --live-dump /tmp/f
```

`/tmp/f-000.png` … `f-015.png`: going to page 1, "LIVE" and "VU meter" slide across the screen;
coming back, the red mic-off icon and red text show in frames 9-14.

## Cause (compared with androidx remote-core)

Both players give a GONE child of a column the position of the slot it would occupy, every
frame (Java `ColumnLayout.java` ~466-471, C++ `LayoutOperations.cpp` ~2111-2116), so a GONE
child's bounds change when its siblings switch.

- **Java:** `Component.layout` creates an `AnimateMeasure`, but `AnimateMeasure.paint`
  (`AnimateMeasure.java` ~146, ~326) draws only when the visibility changes or the target is
  visible. GONE → GONE draws nothing and the component stays GONE.
- **C++:** `animUpdate` (`include/rccore/animation/LayoutAnimation.h` ~144-162) starts an
  animation that is neither entering nor exiting, so it takes the move branch (~196-203), which
  sets `cur.visibility = VIS_VISIBLE` and `alpha = 1`. The GONE component is painted, opaque,
  sliding between its two GONE slots (symptom 2). The branch then stores `st.live = cur`
  (~217), recording the component as VISIBLE. When it really becomes VISIBLE later, `entering`
  (~171) is false, because `from = st.live` is VISIBLE, so it slides in from a stale slot instead
  of fading in (symptom 1). `animateLayoutTree` (`LayoutOperations.cpp` ~3587-3597) then gives
  its children `ANIM_FROZEN` instead of `ANIM_HIDDEN`.

## Suggested fix

In `animUpdate`, before the restart block (~143), snap a GONE → GONE change without animating:

```cpp
if (!target.isVisible() && !st.live.isVisible()) {
    st.animating = false;
    st.live = target;
    target.alpha = 0;
    return false;
}
```

An exit fade already running is unaffected, because its `st.live` is VISIBLE. Belt and braces:
in the move branch, take the visibility from `st.to` (`alpha = st.to.isVisible() ? 1 : 0`)
instead of forcing VISIBLE.

Secondary: every change of destination restarts the animation at `startTime = timeSec` (~159).
Java's `updateTarget` (`AnimateMeasure.java` ~377-411) re-bases the start on the current value
and keeps the start time, so an exit fade whose slot moves doesn't jump back to opaque.

## The remaining slide was the document's

With the fix, the remote still drifted while cross-fading: each screen's column shrank to
nothing when all its blocks went GONE, and a column's own move is animated (Java does the same:
`Component.layout` animates any bounds change that `getAllowsAnimation` allows). The columns in
`tools/knob/knob_fx.json` now fill the screen and centre their content, so their bounds never
change and only the blocks inside fade.
