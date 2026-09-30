# Fuscer Protocol, Version 1

TCP. Each message is a single frame:

```
offset  size  field
0       4     magic "FSCR"
4       1     protocol version (1)
5       1     message type
6       2     flags (0, reserved)
8       4     body length N (big-endian, N <= 256 KiB)
12      N     body
12+N    32    HMAC-SHA256(key, header || body)
```

The body consists of text strings `key=value\n`. Newlines inside a value are replaced with a space; NUL bytes are forbidden.
A message with an invalid HMAC is discarded and the connection is closed.

## Message types

| code | name | direction | body |
|---|---|---|---|
| 1 | HELLO | agent → coordinator | `name`, `interval`, `os`, `kernel`, `distro`, `arch`, `cores`, `ram_mb`, `gpu`, `tools`, `load` |
| 2 | HELLO_OK | coordinator → agent | `coordinator`, `node`, `heartbeat_timeout` |
| 3 | HEARTBEAT | agent → coordinator | `load` |
| 4 | HEARTBEAT_OK | coordinator → agent | empty |
| 5 | STATUS_REQ | client → coordinator | empty |
| 6 | STATUS_RESP | coordinator → client | `version`, `uptime`, `nodes`, then `n<i>.<field>` for each node |
| 7 | ERROR | any | `reason` |
| 8 | EXEC_REQ | client → coordinator | `node`, `timeout` (1..3600 s), `cmd` (hex) |
| 9 | EXEC_RUN | coordinator → agent | `id`, `timeout`, `cmd` (hex) |
| 10 | EXEC_OUT | agent → coordinator → client | `id`, `data` (hex, up to 16 KiB of output per message) |
| 11 | EXEC_DONE | agent → coordinator → client | `id`, `exit`, `signal`, `reason`, `error`, `ms` |
| 12 | EXEC_KILL | coordinator → agent | `id` |

Values cannot contain newlines or NUL bytes, so binary-safe data (the command, its output) is hex-encoded. `cmd` is at most 60000 bytes decoded.

## Sessions

- **Agent:** HELLO → HELLO_OK, then every `interval` seconds HEARTBEAT → HEARTBEAT_OK. The connection is long-lived.
- **Client (`status`):** STATUS_REQ → STATUS_RESP, the connection is closed.
- If an agent with the same name reconnects, the new connection replaces the old one.

- **Exec client:** EXEC_REQ → any number of EXEC_OUT → one EXEC_DONE, then the connection is closed. If the client closes the connection early, the coordinator sends EXEC_KILL to the agent. Refusals (`no such node`, `node is offline`, `node is busy`, exec disabled) arrive as an EXEC_DONE with `error` set and `id=0`; a malformed request gets ERROR.
- **Exec on the agent:** the coordinator sends EXEC_RUN over the agent's existing connection (the agent connects out; the coordinator never dials in). The agent answers with EXEC_OUT while the command runs and EXEC_DONE at the end; heartbeats continue meanwhile. The agent makes its own decision: with exec off it answers EXEC_DONE with `error` and runs nothing, whatever the coordinator says. Only one command per node at a time.
- EXEC_DONE fields: `exit` is the exit code, or -1 if it was killed by a signal or never ran; `signal` is the signal number (0 if none); `reason` is empty, `timeout`, `killed` or `output limit`; `ms` is the run time.
- The HELLO field `exec` (`1`/`0`) says whether the agent accepts commands; the coordinator uses it to refuse early and shows it in `status`.

Protocol version stays 1: the new types are additions, and a 0.1 agent works with a 0.2 coordinator (it simply never gets exec).

## Known limitations

No replay protection (no nonce/counter), no encryption. Both are planned for later. With exec this matters more: a captured EXEC_REQ can be replayed against the coordinator, and a captured EXEC_RUN against an agent by anyone who can impersonate the coordinator. A per-connection challenge plus a message counter under the HMAC would close both.