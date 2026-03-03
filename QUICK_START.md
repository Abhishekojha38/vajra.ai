# Aham — Running Guide

---

## Quick Start

### 1. Build

```bash
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev libreadline-dev

git clone https://github.com/aham-ai/aham
cd aham
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cd ..
```

### 2. Configure

All runtime settings live in `aham.conf`. Open it and set your provider, model, and any paths.

For remote providers, copy `.env.example` to `.env` and add your API key, that's the only secret Aham needs outside of `aham.conf`:

```bash
cp .env.example .env
# Uncomment and set API_KEY=<your key>
```

### 3. Run

```bash
# Local (Ollama — no API key needed)
curl -fsSL https://ollama.com/install.sh | sh
ollama serve & ollama pull mistral-nemo:latest

# set type/model and ollama_url in aham.conf
./build/bin/aham

# Remote provider — set type/model in aham.conf, key in .env, then:
./build/bin/aham

```

### 4. Daemon mode

```bash
./build/bin/aham --daemon --port 8080

curl -X POST http://localhost:8080/api/chat \
     -H 'Content-Type: application/json' \
     -d '{"message":"Check disk usage"}' | jq .response

# Browser
open http://localhost:8080
```

### 5. Useful commands

```bash
./build/bin/aham --log-level info     # see startup detail
./build/bin/aham --log-level debug    # trace every LLM call

# Inside the CLI
/help     # list slash commands
/status   # show provider + model
/tools    # list registered tools
/quit     # exit
```

---

## All Run Options

| Mode | Command | Provider |
|---|---|---|
| Local CLI (Ollama) | `./build/bin/aham` | libcurl → Ollama |
| Local CLI (remote) | `./build/bin/aham` | libcurl → Groq / OpenAI / … |

---

## Local CLI, Ollama

No API key needed. Aham talks directly to Ollama running on your machine.

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama serve &
ollama pull mistral-nemo:latest
./build/bin/aham
```

Configure a different model in `aham.conf`:

```ini
[provider]
type  = ollama
model = qwen2.5:32b
```

Or as a one-shot override:

```bash
MODEL=llama3.2:latest ./build/bin/aham
```

---

## Local CLI, remote provider

Set provider, model, and API key — that's it. Known providers have built-in URLs.

`aham.conf`:
```ini
[provider]
type  = groq
model = llama-3.3-70b-versatile
```

`.env`:
```bash
API_KEY=gsk_...
```

| Provider | `type` value | Notes |
|---|---|---|
| Ollama | `ollama` | Local, no key |
| OpenAI | `openai` | `gpt-4o`, `gpt-4o-mini` |
| Groq | `groq` | Fast inference, free tier |
| OpenRouter | `openrouter` | 200+ models |
| Together AI | `together` | Open-weight models |
| vLLM / llama.cpp | `openai` + `api_url` | Self-hosted, no key |

---

## Configuration

Settings are read in this priority order:

```
1. Shell export     →  export API_KEY=gsk_...
2. .env             →  API_KEY=gsk_...          (secrets only)
3. aham.conf       →  type = groq, model = …         (everything else)
4. Built-in default
```

### aham.conf sections

| Section | Key | Description |
|---|---|---|
| `[provider]` | `type` | `ollama` \| `groq` \| `openrouter` \| `openai` \| `together` |
| `[provider]` | `model` | Model name (required) |
| `[provider]` | `api_key` | API key (or set `API_KEY` in `.env`) |
| `[provider]` | `ollama_url` | Ollama base URL (default: `http://localhost:11434`) |
| `[provider]` | `api_url` | Override URL for self-hosted / proxy endpoints |
| `[agent]` | `max_iterations` | Max tool-call iterations per turn (default: 10) |
| `[agent]` | `max_messages` | History window size; 0 = 128 (default: 0) |
| `[gateway]` | `port` | HTTP daemon port (default: 8080) |
| `[paths]` | `workspace` | Workspace root (default: `.`) |
| `[security]` | `allowlist` | Allowlist config path (default: `allowlist.conf`) |
| `[logging]` | `level` | `debug` \| `info` \| `warn` \| `error` (default: `warn`) |
| `[logging]` | `file` | Append logs to this path (optional) |

### Shell-only env vars

These are only useful to set per-invocation in the shell, not in `.env`:

| Variable | Description |
|---|---|
| `CONFIG` | Path to config file (default: `aham.conf`) |
| `ENV_FILE` | Path to .env file (default: `.env`) |
| `LOG_LEVEL` | Override log level without editing aham.conf |
| `LOG_FILE` | Append logs to this path |

---

## Tracing and debugging

```bash
./build/bin/aham --log-level info     # startup detail
./build/bin/aham --log-level debug    # trace every LLM call + HTTP request

# Or permanently in aham.conf:
# [logging]
# level = debug

```
