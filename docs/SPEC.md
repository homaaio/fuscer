# fuscer

A distributed environment for developing large C/C++ projects across several ordinary computers on a local network: builds, tests, code analysis, and (optionally) local LLMs.

fuscer is **not** "several laptops as one virtual computer." It is a task queue and a set of executors: each machine takes the work it can handle.

## Principles

1. **LAN-first.** Nodes communicate directly over TCP. The internet is not needed for the cluster to operate (it is needed only for GitHub and cloud models, and that is a separate story).
2. **The core does not depend on AI.** The cluster is useful even without LLMs: distributed builds, tests, static and dynamic analysis. An LLM is just another type of task.
3. **Different hardware is normal, the OS is one: Linux.** All nodes run Linux (different distributions and architectures are acceptable). The agent describes its own capabilities, the task describes its requirements, and the coordinator matches them.
4. **Sources travel via Git, not via file transfer.** A task references a commit; the agent does a fetch and checkout. This also works on an offline LAN (the repository can be served from the coordinator).
5. **Humans make the decisions.** The cluster never pushes to the main branch on its own.
6. **Minimum dependencies.** Everything should build on an old laptop without exotica.

## Components

```
   Client (CLI / script / AI agent)
                │
                ▼
          ┌───────────┐
          │Coordinator│  queue, scheduler, state, logs
          └─────┬─────┘
       ┌────────┼────────┐
       ▼        ▼        ▼
     Agent    Agent    Agent     execute tasks
```

All of this is a single executable `fuscer` with modes:

```
fuscer coordinator          # start the coordinator
fuscer agent                # start an agent on this machine
fuscer submit ...           # submit a task
fuscer status               # nodes and queue
fuscer logs <task-id>       # task logs
```

### Coordinator

- accepts connections from agents and clients;
- maintains a registry of nodes (name, capabilities, state, heartbeat);
- holds the task queue and dependencies between tasks;
- selects a node for a task;
- collects results and logs;
- (later) serves the Git repository over the LAN.

State is kept in memory + a journal on disk; SQLite is brought in when it is genuinely needed (it is written in C and embeds without pain).

### Agent

On startup it registers and reports:

- a unique name (from the config, hostname by default);
- OS and architecture, number of cores, amount of RAM, presence of a GPU;
- available tools (gcc, clang, cmake, make, git, cppcheck, valgrind, llama.cpp, etc. — determined by checking for their presence in PATH);
- current load.

After that it periodically sends a heartbeat and waits for tasks. It executes a task in a separate working directory, with a timeout, and streams the output to the coordinator.

### Tasks

A task is a description of the work plus requirements for the node:

```
task:
    type:     build
    repo:     my-project
    commit:   a1b2c3d
    command:  cmake --build build --target tests
    needs:    os=linux, tool=cmake, cores>=2
    timeout:  600s
    after:    [configure-42]
```

Main types:

| type | what it does |
|---|---|
| `build` | build (cmake/make, gcc/clang) |
| `test` | run tests, collect the result |
| `analyze` | cppcheck, clang-tidy, sanitizers, valgrind |
| `llm` | request to a local model on this node |
| `exec` | arbitrary command (disabled by default) |

Result: status, return code, full log, execution time, the node it ran on.

Cross-platform support is not needed: all nodes are on Linux, so building a single project can happen on any suitable agent (taking architecture and tool availability into account).

### The LLM part

fuscer **does not implement inference**. An agent that has a model launches or uses an existing runtime (for example, llama.cpp via its HTTP server) and wraps it in an `llm` task. Tasks for models: code review, bug hunting, parsing compiler errors, test generation, refactoring suggestions.

A weak node is not required to host a model — it handles builds and tests. Model output is suggestions (text/patches), not changes to the repository.

### Clients

The coordinator is not tied to a specific client. A task can be submitted by a human via the CLI, a script, or an AI assistant that knows how to invoke the CLI. The typical cycle: the client formulates a set of subtasks → the coordinator distributes them → the client collects the results → a human reviews and decides what to commit.

## Protocol

TCP, its own simple binary format: a fixed-size header (version, message type, length) + body. The body is simple text `key=value` pairs, so as not to drag in a JSON parser and to make debugging easy. The format is versioned from day one.

## Security

An agent executes commands over the network, so even on a home LAN:

- a shared secret key, with authentication (HMAC) on every message;
- `exec` is disabled by default, the agent only performs known task types;
- tasks run in a separate working directory, with a timeout and an output limit;
- the coordinator listens only on the LAN interface specified in the config.

Traffic encryption is not in the first version.

## Technologies

- **C (C11)** for the coordinator, agent, and tasks; a careful style without heavy frameworks. C++ only where it is genuinely unavoidable.
- **Linux and POSIX only**: sockets, processes, pthreads — directly, without a portability layer.
- **GCC/Clang**, **Make** (primary build) and **CMake** (fallback).
- Git for delivering sources.

## Roadmap

1. **Hello.** An agent connects to the coordinator, registers, sends heartbeats. `fuscer status` shows the nodes.
2. **Remote exec.** The coordinator sends a command to an agent, receives the output and return code.
3. **Queue and scheduler.** Multiple nodes, node requirements, parallel execution, dependencies between tasks.
4. **Git + build + test.** A task by commit, builds and tests on remote agents, central logs.
5. **Analysis.** cppcheck, clang-tidy, sanitizers, valgrind as task types.
6. **LLM tasks.** Connecting a local runtime, routing tasks by models and nodes.
7. **Convenience.** Node auto-discovery on the LAN (UDP broadcast), web or TUI status, Git serving over the LAN.

A new node joins without architectural changes: install `fuscer`, set a name and the coordinator's address — after that it registers itself and receives suitable tasks.

## Out of scope

- A virtual single machine / shared memory between nodes.
- A custom inference engine.
- Automatically pushing changes to the main branch.
- Working over the internet behind NAT.