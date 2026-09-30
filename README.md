# fuscer v0.1

A distributed environment for C/C++ projects across several Linux machines on a local network.
The full description of the idea is in [docs/SPEC.md](docs/SPEC.md). The protocol format is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## What is in v0.1

This is step 1 of the roadmap ("Hello"):

- `fuscer coordinator` — accepts agents, maintains a node registry;
- `fuscer agent` — registers, reports capabilities (OS, kernel, distro, architecture, CPU cores, RAM, GPU, detected tools), sends heartbeats with load, reconnects on its own after a drop;
- `fuscer status` — a table of nodes and their state (online/offline).

Already present: a versioned binary protocol, authentication of every message (HMAC-SHA256, own implementation — without OpenSSL), limits on message size and number of connections, listening only on the specified interface.

Not yet present: tasks, queue, `exec`, `submit`/`logs` (steps 2+), an on-disk journal, traffic encryption. There is also no protection against replaying intercepted messages (replay) — keep this in mind if the LAN is not trusted.

## Building

Only `gcc` (or `clang`) and `make` are needed.

```
make            # build/fuscer
make test       # unit tests (SHA-256/HMAC, protocol) + integration smoke test on localhost
sudo make install
```

Fallback: `cmake -B build && cmake --build build` (the CMake file is written, but was not tested in the v0.1 development environment — the primary path is `make`).

## Quick start

1. Generate a shared key and put it in the config on **every** machine:

   ```
   head -c 24 /dev/urandom | base64
   cp fuscer.conf.example fuscer.conf && chmod 600 fuscer.conf
   ```

   In `fuscer.conf`, set `key`, as well as `listen` (the IP of the coordinator's LAN interface) and `coordinator` (the same address — for agents and `status`).

2. On the coordinator machine: `./build/fuscer coordinator`
3. On each of the others: `./build/fuscer agent --name laptop-1`
4. From anywhere: `./build/fuscer status`

```
coordinator 0.1.0 at 192.168.1.10:7777, up 2m10s; 2 node(s), 2 online

NODE             STATE    OS/ARCH        CORES     RAM  LOAD GPU     SEEN      TOOLS
laptop-1         online   linux/x86_64       4    7.6G  0.12 none    2s ago    gcc,g++,make,git
old-thinkpad     online   linux/x86_64       2    3.9G  0.40 none    1s ago    gcc,make,git,cppcheck
```

The key can also be passed via `FUSCER_KEY`. It cannot be passed via a command-line flag (it would be visible in `ps`).

## Settings

| parameter | where | default |
|---|---|---|
| `key` | all | — (required) |
| `listen` | coordinator | `127.0.0.1:7777` |
| `coordinator` | agent, status | `127.0.0.1:7777` |
| `name` | agent | hostname |
| `interval` | agent | 5 s |

Order: defaults → `./fuscer.conf` or `/etc/fuscer.conf` (or `-c FILE`) → `FUSCER_KEY` → flags (`--listen`, `--connect`, `--name`, `--interval`).

A node is considered offline immediately when the connection drops, and if the connection is "hung" — after `max(15 s, 3 × interval)` without a heartbeat.

## Structure

```
src/main.c         command and settings parsing
src/coordinator.c  node registry, thread per connection
src/agent.c        registration, heartbeat, reconnection
src/status.c       the `fuscer status` client
src/proto.[ch]     frames, HMAC, key=value
src/sha256.[ch]    SHA-256 and HMAC-SHA256
src/net.[ch]       TCP (listen/connect/timeouts)
src/sysinfo.[ch]   node capabilities from /proc, uname, PATH
src/config.[ch]    config file
tests/             test_proto.c, smoke.sh
```