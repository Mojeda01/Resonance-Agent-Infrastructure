# Resonance Agent Infrastructure 

![Language: C](https://img.shields.io/badge/language-C-A8B9CC?logo=c&logoColor=white)
![Inference: llama.cpp](https://img.shields.io/badge/inference-llama.cpp-black)
![Models: GGUF](https://img.shields.io/badge/models-GGUF-blue)
![Transport: Unix socket](https://img.shields.io/badge/transport-Unix%20socket-informational)
![Platform: macOS | Linux](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)
![Status: WIP](https://img.shields.io/badge/status-WIP-orange)

Resonance Agent Infrastructure is a local LLM inference and agent runtime written in C on top of llama.cpp. It runs as a long-lived daemon that scans a model directory, loads GGUF models on request, and talks to clients over a Unix domain socket using a compact typed binary protocol. The protocol covers listing and loading models, prompting, cancelling, resetting, tailing a session's output, and dropping sessions. Each conversation gets its own session backed by a separate sequence in the llama.cpp KV cache, so context carries across turns without re-evaluating history. Responses stream back token by token along with time-to-first-token and tokens-per-second stats. A filter strips hidden <think> reasoning spans as the stream arrives, even when a tag is split across tokens. The agent loop is built into the generation path. The model emits plain-text TOOL: calls, the runtime pauses generation and executes them (read a file, write a file, or run a shell command with output caps, a timeout and process-group kill), feeds the result back into the live context, and resumes up to a fixed number of hops. A skills prompt is injected on the first turn, and the loop guards against the model writing past its own turn.


## Build & run

Requires CMake ≥ 3.16, a C11 compiler, and llama.cpp (`brew install llama.cpp` on macOS, or build it yourself).

```sh
cmake -B build -S .            # or: cmake -B build -S . -DLLAMA_CPP_DIR=/path/to/llama.cpp
cmake --build build -j
```

This produces two binaries in `build/`:

```sh
# interactive agent / one-shot CLI (run from the project root so src/skills is found,
# or export AGENT_SKILLS_DIR=/path/to/skills)
./build/agent -m /path/to/model.gguf

# daemon: scans a directory of .gguf models and serves a Unix socket
./build/agentd -d /path/to/models -S /tmp/agentinfra.sock
```

`agentd` defaults to `/run/agentinfra.sock`, which usually needs root, so pass `-S` with a user-writable path.
