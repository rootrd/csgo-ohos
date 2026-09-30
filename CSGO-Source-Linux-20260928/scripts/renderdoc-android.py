"""qrenderdoc --python driver for scripts/renderdoc-android.sh.

qrenderdoc does not show this script's stdout, so everything goes to CSGO_RD_LOG.
"""

import os
import time
import traceback

import renderdoc as rd

log_file = open(os.environ["CSGO_RD_LOG"], "w")


def log(*parts):
    print(*parts, file=log_file, flush=True)


def capture():
    output = os.environ["CSGO_RD_CAPTURE"]
    control, seen = None, []
    for port in os.environ["CSGO_RD_PORTS"].split():
        candidate = rd.CreateTargetControl("localhost", int(port), "csgo-capture", False)
        if candidate is None or not candidate.Connected():
            continue
        if str(candidate.GetPID()) == os.environ["CSGO_RD_PID"]:
            control = candidate
            break
        seen.append("%s PID %d" % (candidate.GetTarget(), candidate.GetPID()))
        candidate.Shutdown()
    if control is None:
        raise RuntimeError("No RenderDoc target is the game process %s (found: %s)." %
                           (os.environ["CSGO_RD_PID"], ", ".join(seen) or "none"))
    try:
        log("Target", control.GetTarget(), "API", control.GetAPI(), "PID", control.GetPID())
        # The connection first announces older captures; none of them may count as this one.
        previous = set()
        settle = time.monotonic() + 1.0
        while time.monotonic() < settle:
            message = control.ReceiveMessage(None)
            if message.type == rd.TargetControlMessageType.NewCapture:
                previous.add(message.newCapture.captureId)
        control.TriggerCapture(1)
        pending = None
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            message = control.ReceiveMessage(None)
            if not control.Connected():
                raise RuntimeError("The game closed the RenderDoc connection.")
            if message.type == rd.TargetControlMessageType.NewCapture and pending is None \
                    and message.newCapture.captureId not in previous:
                pending = message.newCapture.captureId
                log("Captured frame", message.newCapture.frameNumber, "bytes", message.newCapture.byteSize)
                control.CopyCapture(pending, output)
            elif message.type == rd.TargetControlMessageType.CaptureCopied \
                    and message.newCapture.captureId == pending:
                log("Copied", output)
                return
            else:
                time.sleep(0.05)
        raise RuntimeError("Timed out waiting for the capture; the game may not have presented a frame.")
    finally:
        control.Shutdown()


def replay():
    for _ in range(40):
        status, remote = rd.CreateRemoteServerConnection(os.environ["CSGO_RD_REMOTE"])
        if status.OK():
            break
        time.sleep(0.5)
    if not status.OK():
        raise RuntimeError("RenderDoc remote server connection failed: " + status.Message())
    try:
        options = rd.ReplayOptions()
        options.optimisation = rd.ReplayOptimisationLevel.Fastest
        # 0xFFFFFFFF is NoPreference; not every Python binding exports the constant.
        status, controller = remote.OpenCapture(0xFFFFFFFF, os.environ["CSGO_RD_REMOTE_FILE"], options, None)
        if not status.OK():
            raise RuntimeError("Remote replay failed to open the capture: " + status.Message())
        try:
            script = os.environ["CSGO_RD_SCRIPT"]
            with open(script) as source:
                code = compile(source.read(), script, "exec")
            exec(code, {"rd": rd, "controller": controller, "log": log, "__name__": "__main__"})
        finally:
            remote.CloseCapture(controller)
    finally:
        remote.ShutdownConnection()


status = 1
try:
    {"capture": capture, "replay": replay}[os.environ["CSGO_RD_MODE"]]()
    status = 0
except Exception:
    log(traceback.format_exc())
finally:
    log_file.close()
    os._exit(status)
