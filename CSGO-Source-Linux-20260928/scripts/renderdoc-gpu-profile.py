# Replay script for `renderdoc-android.sh replay file.rdc scripts/renderdoc-gpu-profile.py`.
# Groups instrumented EventGPUDuration counters by render scope. Their sum is
# not a whole-pass GPU timestamp: tile rendering and pass-boundary work may be
# absent. Use r_csm_profile for whole-CSM timing and android-perf.py for FPS.
import json
import os
from pathlib import Path

import renderdoc as rd

counters = controller.EnumerateCounters()
if rd.GPUCounter.EventGPUDuration not in counters:
    raise RuntimeError("EventGPUDuration is not available on this replay device")
durations = {r.eventId: r.value.d for r in controller.FetchCounters([rd.GPUCounter.EventGPUDuration])}
sfile = controller.GetStructuredFile()


def flat(actions):
    for action in actions:
        yield action
        yield from flat(action.children)


actions = list(flat(controller.GetRootActions()))
descs = {texture.resourceId: texture for texture in controller.GetTextures()}


def describe(target):
    texture = descs.get(target.resource)
    return f"{texture.width}x{texture.height}:{texture.format.Name()}" if texture else "?"


passes, events, current, total = [], [], None, 0.0
for action in actions:
    name = action.GetName(sfile)
    # Markers can contain the same work as their children. Sum only leaf events.
    t = 0.0 if action.children else durations.get(action.eventId, 0.0)
    total += t
    if "BeginRendering" in name or "BeginRenderPass" in name:
        current = {"event": action.eventId, "name": name, "time": 0.0, "draws": 0, "clears": 0, "targets": ""}
        passes.append(current)
        controller.SetFrameEvent(action.eventId, False)
        state = controller.GetPipelineState()
        outs = [r for r in state.GetOutputTargets() if r.resource != rd.ResourceId.Null()]
        depth = state.GetDepthTarget()
        current["targets"] = " ".join(describe(r) for r in outs) + (" depth " + describe(depth) if depth.resource != rd.ResourceId.Null() else "")
        continue
    if action.children:
        continue
    if current is None:
        current = {"event": action.eventId, "name": "(outside rendering)", "time": 0.0, "draws": 0, "clears": 0, "targets": ""}
        passes.append(current)
    current["time"] += t
    if action.flags & rd.ActionFlags.Drawcall:
        current["draws"] += 1
    if action.flags & rd.ActionFlags.Clear:
        current["clears"] += 1
    if t:
        events.append({"event": action.eventId, "name": name, "duration_ms": t * 1000,
                       "pass": current["event"], "draw": bool(action.flags & rd.ActionFlags.Drawcall),
                       "indices": action.numIndices, "instances": action.numInstances})
    if "EndRendering" in name or "EndRenderPass" in name:
        current = None

log("Event-counter sums below are not measured whole-pass durations; do not extrapolate FPS from them.")
draw_times = [e["duration_ms"] for e in events if e["draw"]]
if draw_times and max(draw_times) <= min(draw_times) * 1.10:
    log("WARNING: All draw counters lie within 10% of one another. On tile GPUs these may describe "
        "query/command overhead rather than the full rendering work.")
log(f"instrumented replay GPU event sum {total * 1000:.2f} ms, {len(passes)} scopes, "
    f"{sum(p['draws'] for p in passes)} draws")
for p in sorted(passes, key=lambda p: -p["time"])[:40]:
    log(f"{p['time'] * 1000:7.3f} ms  draws {p['draws']:4d} clears {p['clears']:2d}  eid {p['event']:6d}  {p['targets']}")
log("-- chronological --")
for p in passes:
    if p["time"] * 1000 >= 0.05:
        log(f"eid {p['event']:6d} {p['time'] * 1000:7.3f} ms draws {p['draws']:4d} {p['targets']}")
log("-- costliest draws --")
for event in sorted((e for e in events if e["draw"]), key=lambda e: -e["duration_ms"])[:25]:
    log(f"eid {event['event']:6d} {event['duration_ms']:7.3f} ms "
        f"indices {event['indices']:7d} instances {event['instances']:4d} {event['name'][:120]}")
log("-- costliest copies, clears and other GPU events --")
for event in sorted((e for e in events if not e["draw"]), key=lambda e: -e["duration_ms"])[:25]:
    log(f"eid {event['event']:6d} {event['duration_ms']:7.3f} ms {event['name'][:140]}")

if os.environ.get("CSGO_RD_PROFILE_JSON"):
    output = Path(os.environ["CSGO_RD_PROFILE_JSON"])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"instrumented_replay_gpu_ms": total * 1000,
                                  "timing_semantics": "sum of instrumented event counters; not whole-pass GPU duration",
                                  "scopes": passes, "events": events}, indent=2) + "\n")
    log("Saved", output)
