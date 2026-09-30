#!/bin/sh
# Integration smoke test: coordinator + agents on localhost.
# usage: tests/smoke.sh path/to/fuscer
set -u

BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$BIN" ] || { echo "no binary: $1"; exit 2; }

PORT=$((17000 + $$ % 2000))
ADDR=127.0.0.1:$PORT
KEY=smoke-test-key-0123456789
DIR=$(mktemp -d)
cd "$DIR" || exit 2       # so a stray ./fuscer.conf is not picked up
PIDS=""

cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$DIR"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $1"
    echo "--- coordinator log ---"; cat coord.log 2>/dev/null
    echo "--- last status ---";     cat status.out 2>/dev/null
    exit 1
}

status() { FUSCER_KEY=$KEY "$BIN" status --connect "$ADDR" >status.out 2>&1; }

start_coord() {
    FUSCER_KEY=$KEY "$BIN" coordinator --listen "$ADDR" >>coord.log 2>&1 &
    COORD=$!; PIDS="$PIDS $COORD"
    sleep 0.5
}
start_agent() {  # name key
    FUSCER_KEY=$2 "$BIN" agent --connect "$ADDR" --name "$1" --interval 1 >"agent-$1.log" 2>&1 &
    eval "AGENT_$1=$!"; PIDS="$PIDS $!"
}

# wait_for <pattern> <seconds>: poll status until pattern appears
wait_for() {
    i=0
    while [ $i -lt $(($2 * 5)) ]; do
        status; grep -Eq "$1" status.out && return 0
        sleep 0.2; i=$((i + 1))
    done
    return 1
}

start_coord
start_agent alpha "$KEY"
start_agent beta  "$KEY"

wait_for '2 online' 5              || fail "two agents did not register"
grep -Eq '^alpha +online +linux/' status.out || fail "alpha row missing"
grep -Eq '^beta +online'          status.out || fail "beta row missing"
echo "ok: registration + status"

start_agent gamma "wrong-key-wrong-key-x"
sleep 2
status
grep -q gamma status.out && fail "agent with wrong key was registered"
grep -q 'authentication failed' coord.log || fail "no auth failure logged"
echo "ok: wrong-key agent rejected"

FUSCER_KEY=wrong-key-wrong-key-x "$BIN" status --connect "$ADDR" >/dev/null 2>&1 \
    && fail "status with wrong key succeeded"
echo "ok: wrong-key status rejected"

kill "$AGENT_beta"
wait_for '^beta +offline' 3        || fail "beta not marked offline"
grep -q '1 online' status.out      || fail "online count wrong after kill"
echo "ok: offline detection"

start_agent beta "$KEY"
wait_for '^beta +online' 4         || fail "beta did not come back"
echo "ok: re-registration"

kill "$COORD"; wait "$COORD" 2>/dev/null
start_coord
wait_for '2 online' 15             || fail "agents did not reconnect after coordinator restart"
echo "ok: reconnect after coordinator restart"

echo "smoke: all ok"
