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
start_agent() {  # name key [extra agent flags]
    _n=$1; _k=$2; shift 2
    FUSCER_KEY=$_k "$BIN" agent --connect "$ADDR" --name "$_n" --interval 1 "$@" >"agent-$_n.log" 2>&1 &
    eval "AGENT_$_n=$!"; PIDS="$PIDS $!"
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

echo "exec = yes" > exec.conf      # only alpha may run remote commands
start_coord
start_agent alpha "$KEY" -c exec.conf
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

# ---------------------------------------------------------------- exec
fx() { FUSCER_KEY=$KEY "$BIN" exec --connect "$ADDR" "$@"; }
# gone <pid>: wait until the process is gone (or only a zombie)
gone() {
    i=0
    while [ $i -lt 25 ]; do
        st=$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' "/proc/$1/status" 2>/dev/null)
        { [ -z "$st" ] || [ "$st" = Z ]; } && return 0
        sleep 0.2; i=$((i + 1))
    done
    return 1
}

wait_for '^alpha +online.* yes +[0-9]+s ago' 5 || fail "alpha should show exec=yes"
grep -Eq '^beta +online.* - +[0-9]+s ago' status.out || fail "beta should show exec=-"
echo "ok: status shows which nodes accept exec"

out=$(fx alpha -- echo hello 2>&1); rc=$?
[ $rc -eq 0 ] && [ "$out" = hello ] || fail "exec echo: rc=$rc out=$out"
echo "ok: exec runs a command and returns its output"

fx alpha 'exit 7' >/dev/null 2>&1; rc=$?
[ $rc -eq 7 ] || fail "exit code not passed through: $rc"
fx alpha 'kill -9 $$' >/dev/null 2>&1; rc=$?
[ $rc -eq 137 ] || fail "death by signal should give 128+9, got $rc"
echo "ok: exit status and signals"

out=$(fx alpha 'echo one; echo two >&2; printf three' 2>&1)
[ "$out" = "$(printf 'one\ntwo\nthree')" ] || fail "stdout/stderr or newlines mangled: [$out]"
out=$(fx alpha -- printf '[%s]' "it's" 'a b' '$HOME' 2>&1)
[ "$out" = "[it's][a b][\$HOME]" ] || fail "argv quoting broken: [$out]"
out=$(fx alpha 'cat; echo eof')
[ "$out" = eof ] || fail "stdin should be empty, got [$out]"
echo "ok: output, argv quoting, stdin"

n=$(fx alpha 'seq 1 200000' | wc -l)
[ "$n" -eq 200000 ] || fail "large output truncated: $n lines"
echo "ok: large output"

fx alpha env | grep -q "$KEY" && fail "the cluster key leaked into the command's environment"
echo "ok: key not leaked to commands"

fx beta -- true >exec.out 2>&1; rc=$?
[ $rc -eq 125 ] && grep -q 'disabled' exec.out || fail "exec on a node with exec off should be refused ($rc)"
grep -q 'refused' agent-beta.log 2>/dev/null && fail "coordinator should stop it before the agent"
fx nosuch -- true >exec.out 2>&1; rc=$?
[ $rc -eq 125 ] && grep -q 'no such node' exec.out || fail "unknown node ($rc)"
FUSCER_KEY=wrong-key-wrong-key-x "$BIN" exec --connect "$ADDR" alpha -- true >/dev/null 2>&1; rc=$?
[ $rc -eq 125 ] || fail "exec with the wrong key should fail ($rc)"
echo "ok: refusals (exec off, unknown node, wrong key)"

t0=$(date +%s)
fx --timeout 1 alpha 'sleep 30' >exec.out 2>&1; rc=$?
t1=$(date +%s)
[ $rc -eq 124 ] && [ $((t1 - t0)) -le 8 ] || fail "timeout: rc=$rc after $((t1 - t0))s"
fx --timeout 1 alpha 'sleep 31 & echo $! > bg.pid; wait' >/dev/null 2>&1; rc=$?
[ $rc -eq 124 ] || fail "timeout with background child: rc=$rc"
gone "$(cat bg.pid)" || fail "background child survived the timeout"
echo "ok: timeout kills the whole process group"

t0=$(date +%s)
fx alpha 'sleep 32 & echo $! > bg2.pid' >/dev/null 2>&1; rc=$?
t1=$(date +%s)
[ $rc -eq 0 ] && [ $((t1 - t0)) -le 4 ] || fail "command with a background child hung ($rc, $((t1 - t0))s)"
gone "$(cat bg2.pid)" || fail "leftover background process was not cleaned up"
echo "ok: leftover background processes are cleaned up"

fx alpha 'sleep 3' >/dev/null 2>&1 & BG=$!
sleep 0.8
fx alpha -- true >exec.out 2>&1; rc=$?
[ $rc -eq 125 ] && grep -q 'busy' exec.out || fail "second exec on a busy node ($rc)"
wait "$BG" || fail "the first job should have succeeded"
fx alpha -- true || fail "node should be free again"
echo "ok: one command per node"

fx alpha 'sleep 4' >/dev/null 2>&1 & BG=$!
sleep 2.5
status
grep -E '^alpha +online' status.out | grep -Eq '[0-2]s ago' || fail "heartbeats stopped during exec"
wait "$BG"
echo "ok: heartbeats continue while a command runs"

# (run the binary directly: $! of a shell function would be a subshell)
FUSCER_KEY=$KEY "$BIN" exec --connect "$ADDR" alpha 'echo $$ > kill.pid; exec sleep 33' >/dev/null 2>&1 & CP=$!
sleep 1.2
kill "$CP"; wait "$CP" 2>/dev/null
gone "$(cat kill.pid)" || fail "remote command survived the client's exit"
sleep 0.5
fx alpha -- true || fail "node stayed busy after the client left"
echo "ok: client disconnect kills the remote command"

FUSCER_KEY=$KEY "$BIN" exec --connect "$ADDR" alpha 'echo $$ > kill2.pid; exec sleep 34' >/dev/null 2>&1 & CP=$!
sleep 1.2
kill "$AGENT_alpha"; wait "$AGENT_alpha" 2>/dev/null
wait "$CP" 2>/dev/null
gone "$(cat kill2.pid)" || fail "command survived the agent's shutdown"
wait_for '^alpha +offline' 4 || fail "alpha should be offline"
fx alpha -- true >exec.out 2>&1; rc=$?
[ $rc -eq 125 ] && grep -q 'offline' exec.out || fail "exec on an offline node ($rc)"
echo "ok: agent shutdown kills the command; offline node refused"

echo "smoke: all ok"
