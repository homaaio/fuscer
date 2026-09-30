# fuscer v0.2

A distributed environment for C/C++ projects across several Linux machines on a local network.
The full description of the idea is in [docs/SPEC.md](docs/SPEC.md). The protocol format is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## What is in v0.2

Steps 1 and 2 of the roadmap ("Hello" and "Remote exec"):

- `fuscer coordinator` — accepts agents, maintains a node registry, relays commands and their output;
- `fuscer agent` — registers, reports capabilities (OS, kernel, distro, architecture, CPU cores, RAM, GPU, detected tools), sends heartbeats with load, reconnects on its own after a drop, and (only if `exec = yes` in its config) runs commands;
- `fuscer status` — a table of nodes, their state and whether they accept exec;
- `fuscer exec NODE CMD...` — run a command on a node; its output streams back, and the exit status is the command's.

Already present: a versioned binary protocol, authentication of every message (HMAC-SHA256, own implementation — without OpenSSL), limits on message size and number of connections, listening only on the specified interface.

Not yet present: a task queue and scheduler, `submit`/`logs` (steps 3+), an on-disk journal, traffic encryption, protection against replay.

**Read this before enabling exec.** `exec = yes` lets anyone who holds the shared key run any command on that machine as the agent's user. The protocol has no replay protection yet, so on a LAN where someone can capture traffic, a recorded `exec` request could be replayed later. Enable it only on a LAN you trust, use an unprivileged user for the agent, and keep `fuscer.conf` at mode 600. Replay protection is the first thing to add before this goes anywhere less private.

## Building

Only `gcc` (or `clang`) and `make` are needed.

```
make            # build/fuscer
make test       # unit tests (SHA-256/HMAC, protocol, hex) + integration smoke test on localhost
sudo make install
```

Fallback: `cmake -B build && cmake --build build` (the CMake file is written, but was not tested in the development environment — the primary path is `make`).

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
5. To run commands on a node, put `exec = yes` in **that node's** `fuscer.conf`, restart its agent, and:

   ```
   ./build/fuscer exec laptop-1 -- gcc --version
   ./build/fuscer exec --timeout 300 old-thinkpad 'make -j2 2>&1 | tail -20'
   ```

```
coordinator 0.2.0 at 192.168.1.10:7777, up 2m10s; 2 node(s), 2 online

NODE             STATE    OS/ARCH        CORES     RAM  LOAD GPU     EXEC SEEN      TOOLS
laptop-1         online   linux/x86_64       4    7.6G  0.12 none    yes  2s ago    gcc,g++,make,git
old-thinkpad     online   linux/x86_64       2    3.9G  0.40 none    -    1s ago    gcc,make,git,cppcheck
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
| `exec` | agent | `no` (`yes` to accept remote commands) |

Order: defaults → `./fuscer.conf` or `/etc/fuscer.conf` (or `-c FILE`) → `FUSCER_KEY` → flags (`--listen`, `--connect`, `--name`, `--interval`).

`exec` is deliberately a config-file setting of the agent only: there is no command-line flag or environment variable for it, so it cannot be switched on by accident, and the coordinator cannot switch it on for a node.

A node is considered offline immediately when the connection drops, and if the connection is "hung" — after `max(15 s, 3 × interval)` without a heartbeat.

## exec

```
fuscer exec [--timeout SEC] NODE COMMAND...
```

- **One argument** is a shell command line, run as `sh -c` on the node: `fuscer exec n1 'make 2>&1 | tail'`.
  **Several arguments** are an argv, passed through exactly (each word is quoted for you): `fuscer exec n1 -- printf '[%s]' "it's" 'a b'`.
- stdout and stderr are merged into one stream, in order, and streamed back as they are produced. stdin is empty.
- The command runs in the agent's working directory, in its own process group. On timeout (default 60 s, at most 3600) it gets SIGTERM, then SIGKILL after 3 s; the whole group is killed. Background processes a command leaves behind are killed when the command exits.
- Output is capped at 4 MiB; beyond that the command is killed.
- Ctrl-C on the client, or losing the connection, kills the remote command. So does the agent shutting down.
- One command per node at a time ("node is busy" otherwise; queueing comes with step 3).
- The `FUSCER_KEY` variable is removed from the command's environment.
- Exit status: the command's own (0-255); 128+N if killed by signal N; **124** timeout; **125** fuscer itself failed (unknown or offline node, exec disabled, network, wrong key); 130 interrupted.

## Structure

```
src/main.c         command and settings parsing
src/coordinator.c  node registry, thread per connection, exec relay
src/agent.c        registration, heartbeat, reconnection, running commands
src/status.c       the `fuscer status` client
src/exec.c         the `fuscer exec` client
src/proto.[ch]     frames, HMAC, key=value, hex
src/sha256.[ch]    SHA-256 and HMAC-SHA256
src/net.[ch]       TCP (listen/connect/timeouts)
src/sysinfo.[ch]   node capabilities from /proc, uname, PATH
src/config.[ch]    config file
tests/             test_proto.c, smoke.sh (includes the exec scenarios)
```