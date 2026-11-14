#!/bin/bash
# The Linux sound output against real servers: starts a headless PipeWire, WirePlumber and
# pipewire-pulse (two null sinks) in a scratch runtime directory and runs cordinal-core-test's
# sound checks through both of Cordinal's clients (native PipeWire, then PulseAudio's protocol).
#
#   tools/audio-check.sh path/to/cordinal-core-test
#
# Needs pipewire, wireplumber, pipewire-pulse, pipewire-bin (pw-record, pw-metadata, pw-link),
# pulseaudio-utils (pactl) and dbus-daemon. Exits non-zero if any check failed.
set -u
TEST=$(readlink -f "${1:?usage: audio-check.sh path/to/cordinal-core-test}")
WORK=$(mktemp -d /tmp/cordinal-audio.XXXXXX)
export XDG_RUNTIME_DIR=$WORK/run XDG_CONFIG_HOME=$WORK/config
unset PIPEWIRE_REMOTE PIPEWIRE_RUNTIME_DIR PULSE_SERVER PULSE_RUNTIME_PATH CORDINAL_AUDIO CORDINAL_EXCLUSIVE
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d" "$XDG_CONFIG_HOME/wireplumber"
chmod 700 "$XDG_RUNTIME_DIR"
cat > "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/10-cordinal-check.conf" <<'CONF'
context.properties = { default.clock.allowed-rates = [ 44100 48000 88200 96000 ] }
context.objects = [
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "test-sink-a"
      node.description = "Test Sink A" media.class = "Audio/Sink" audio.position = [ FL FR ]
      monitor.channel-volumes = true object.linger = true } }
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "test-sink-b"
      node.description = "Test Sink B" media.class = "Audio/Sink" audio.position = [ FL FR ]
      monitor.channel-volumes = true object.linger = true } }
]
CONF

PIDS=()
DBUS_PID=
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-daemon >/dev/null; then
    eval "$(dbus-daemon --session --fork --print-address=1 --print-pid=1 | {
        read -r a; read -r p; echo "export DBUS_SESSION_BUS_ADDRESS='$a'; DBUS_PID=$p"; })"
fi
start_servers() {
    pipewire >> "$WORK/pipewire.log" 2>&1 & PIDS=($!)
    for _ in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] && break; sleep 0.1; done
    wireplumber >> "$WORK/wireplumber.log" 2>&1 & PIDS+=($!)
    pipewire-pulse >> "$WORK/pulse.log" 2>&1 & PIDS+=($!)
    for _ in $(seq 50); do
        pactl list short sinks 2>/dev/null | grep -q test-sink-b && pw-metadata 0 default.audio.sink 2>/dev/null |
            grep -q test-sink && break
        sleep 0.1
    done
    set_default test-sink-a
}
stop_servers() {
    [ ${#PIDS[@]} -gt 0 ] && kill "${PIDS[@]}" 2>/dev/null && wait "${PIDS[@]}" 2>/dev/null
    PIDS=()
    rm -f "$XDG_RUNTIME_DIR/pipewire-0" "$XDG_RUNTIME_DIR/pipewire-0.lock"
}
cleanup() {
    stop_servers
    [ -n "$DBUS_PID" ] && kill "$DBUS_PID" 2>/dev/null
    [ -n "${KEEP:-}" ] && echo "kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT
set_default() { pactl set-default-sink "$1" >/dev/null 2>&1; sleep 0.3; }

FAILED=0
PASSED=0
result() {  # result NAME STATUS [LOG]
    if [ "$2" = 0 ]; then echo "ok   $1"; PASSED=$((PASSED + 1))
    else echo "FAIL $1"; FAILED=$((FAILED + 1)); [ -n "${3:-}" ] && sed 's/^/       /' "$3"; fi
}
# capture SINK RATE FILE: records SINK's monitor as float32 stereo until stop_capture.
capture() {
    pw-record -P stream.capture.sink=true --target "$1" --format f32 --rate "$2" --channels 2 "$3" \
        > "$WORK/record.log" 2>&1 & CAPTURE=$!
    sleep 0.7
}
stop_capture() { sleep 0.4; kill -INT "$CAPTURE" 2>/dev/null; wait "$CAPTURE" 2>/dev/null; }
run() { "$TEST" "$@" > "$WORK/out" 2>&1; }

# A tone longer than the restart check plays (12 s at 48 kHz, 16-bit stereo).
python3 - "$WORK/tone.wav" <<'PY'
import math, struct, sys
rate, n = 48000, 48000 * 12
data = b"".join(struct.pack("<hh", v, v) for v in (int(8000 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(n)))
with open(sys.argv[1], "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16))
    f.write(b"data" + struct.pack("<I", len(data)) + data)
PY

run sound-pod; result "POD builder and reader" $? "$WORK/out"
run sound-fuzz 50000; result "message parsers on 50000 mutated messages" $? "$WORK/out"

start_servers
for backend in pipewire pulse; do
    export CORDINAL_AUDIO=$backend
    echo "--- $backend"
    run devices
    st=$?; grep -q "^test-sink-a" "$WORK/out" && grep -q "^test-sink-b" "$WORK/out" || st=1
    result "$backend: device list" $st "$WORK/out"

    capture test-sink-a 48000 "$WORK/cap.wav"
    run sound-pattern 4; st=$?; cp "$WORK/out" "$WORK/play.out"
    stop_capture
    run sound-compare "$WORK/cap.wav" 48000 3; [ $? = 0 ] || st=1
    cat "$WORK/out" >> "$WORK/play.out"
    result "$backend: the default sink receives the samples bit for bit, position and latency" $st "$WORK/play.out"
    grep -h "x real time\|ahead of the device" "$WORK/play.out" | sed 's/^ */       /'

    capture test-sink-b 48000 "$WORK/cap.wav"
    run sound-pattern 3 48000 test-sink-b; st=$?; cp "$WORK/out" "$WORK/play.out"
    stop_capture
    run sound-compare "$WORK/cap.wav" 48000 2; [ $? = 0 ] || st=1
    cat "$WORK/out" >> "$WORK/play.out"
    result "$backend: a chosen device (test-sink-b) receives the samples bit for bit" $st "$WORK/play.out"

    capture test-sink-a 48000 "$WORK/cap.wav"
    run sound-pattern 4 48000 "" 300; st=$?; cp "$WORK/out" "$WORK/play.out"
    stop_capture
    run sound-compare "$WORK/cap.wav" 48000 2.5 gaps; [ $? = 0 ] || st=1
    cat "$WORK/out" >> "$WORK/play.out"
    result "$backend: a 300 ms underrun: the stream rides it out, the samples stay in order" $st "$WORK/play.out"

    capture test-sink-b 48000 "$WORK/capb.wav"
    "$TEST" sound-pattern 6 > "$WORK/play.out" 2>&1 & P=$!
    sleep 2.5
    set_default test-sink-b
    sleep 0.5
    links=$(pw-link -l 2>/dev/null)
    wait $P; st=$?
    stop_capture
    set_default test-sink-a
    echo "$links" | grep -A1 "^cordinal-pattern:output_FL\|^cordinal-pattern:monitor_FL\|^cordinal-pattern" >> "$WORK/play.out"
    run sound-compare "$WORK/capb.wav" 48000 1.5 gaps; [ $? = 0 ] || st=1
    cat "$WORK/out" >> "$WORK/play.out"
    result "$backend: the default device changes mid-play: the stream follows it to test-sink-b" $st "$WORK/play.out"

    run play "$WORK/tone.wav" 2; st=$?
    grep -q "^playing [12]\." "$WORK/out" || st=1
    result "$backend: the engine plays a file" $st "$WORK/out"

    CORDINAL_E2E=1 run sfx; result "$backend: sound effects" $? "$WORK/out"

    # The server goes away mid-play and comes back: the engine must reconnect and carry on.
    "$TEST" play "$WORK/tone.wav" 9 > "$WORK/play.out" 2>&1 & P=$!
    sleep 2.5
    before=$(grep "^playing" "$WORK/play.out" | tail -1 | awk '{print $2}')
    stop_servers
    sleep 1
    start_servers
    wait $P; st=$?
    after=$(grep "^playing" "$WORK/play.out" | tail -1 | awk '{print $2}')
    echo "       position ${before:-?} s before the restart, ${after:-?} s at the end" >> "$WORK/play.out"
    awk -v b="${before:-0}" -v a="${after:-0}" 'BEGIN { exit !(a >= b + 3) }' || st=1
    result "$backend: the server restarts mid-play: the engine reconnects and the position moves on" $st "$WORK/play.out"
    tail -1 "$WORK/play.out"
done
unset CORDINAL_AUDIO

echo "--- rates (PipeWire)"
# The graph runs at 44.1 kHz while the stream is 48 kHz: Cordinal resamples, the clock still holds.
pw-metadata -n settings 0 clock.force-rate 44100 >/dev/null
sleep 0.3
run sound-pattern 3 48000; st=$?
grep -q "server at 44100 Hz" "$WORK/out" || st=1
result "a 48 kHz stream on a 44.1 kHz graph keeps time" $st "$WORK/out"
grep -h "x real time" "$WORK/out" | sed 's/^ */       /'
# A 44.1 kHz stream on that graph goes through untouched.
capture test-sink-a 44100 "$WORK/cap.wav"
CORDINAL_EXCLUSIVE=1 run sound-pattern 3 44100; st=$?; cp "$WORK/out" "$WORK/play.out"
stop_capture
run sound-compare "$WORK/cap.wav" 44100 2; [ $? = 0 ] || st=1
cat "$WORK/out" >> "$WORK/play.out"
result "a 44.1 kHz stream on a 44.1 kHz graph arrives bit for bit" $st "$WORK/play.out"
pw-metadata -n settings 0 clock.force-rate 0 >/dev/null
sleep 0.3
# Exclusive asks the graph for the stream's rate (node.rate): an idle graph switches to it.
CORDINAL_EXCLUSIVE=1 run sound-pattern 3 88200; st=$?
grep -q "server at 88200 Hz" "$WORK/out" || st=1
result "an exclusive 88.2 kHz stream gets the graph switched to 88.2 kHz" $st "$WORK/out"

echo "$PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
