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

## Sessions

- **Agent:** HELLO → HELLO_OK, then every `interval` seconds HEARTBEAT → HEARTBEAT_OK. The connection is long-lived.
- **Client (`status`):** STATUS_REQ → STATUS_RESP, the connection is closed.
- If an agent with the same name reconnects, the new connection replaces the old one.

## Known limitations of v0.1

No replay protection (no nonce/counter), no encryption. Both are planned for later.