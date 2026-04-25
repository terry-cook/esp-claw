# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP-Claw is Espressif's AI agent framework for IoT devices, running on ESP32-series chips. It implements a full agent loop (sensing → decision-making → execution) on-device, using chat/IM to define device behavior via dynamic Lua scripting and LLM tool calls. Written in C using ESP-IDF.

## Build Commands

Firmware builds use **ESP-IDF v5.5** with the `idf_build_apps` and `esp-bmgr-assist` Python packages.

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Build firmware for a specific board (CI method)
python .gitlab/ci/build_apps.py application/basic_demo \
  --config "=" \
  -t esp32s3 \
  -r 1 -vv \
  --board <board_name>

# Merge binary into single flashable image (after build)
python .gitlab/ci/merge_bin.py

# Both commands expect env vars: EXAMPLE_DIR, EXAMPLE_BOARD, EXAMPLE_TARGET
```

Supported boards: `esp32_S3_DevKitC_1`, `esp32_S3_DevKitC_1_breadboard`, `lilygo_t_display_s3`, `m5stack_cores3`, `esp32_p4_function_ev` (P4 target).

### Docs Site

```bash
cd docs
pnpm install
pnpm run dev        # dev server
pnpm run build      # production build (Astro + Starlight)
```

## Architecture

### Core Agent Loop

The system is event-driven with these layers:

1. **`claw_event_router`** — Central event bus. Ingests IM messages, scheduled events, and internal events; routes them through user-defined JSON rules (`router_rules.json`) to either call a capability, run the agent, execute a Lua script, or send a message.
2. **`claw_core`** — LLM agent loop. Submits requests to the LLM, manages context providers (memory, skills, tools, session history), executes tool calls via `claw_cap`, and returns responses. Supports iterative tool use up to `max_tool_iterations`.
3. **`claw_cap`** — Capability registry. Capabilities (`cap_*`) are registered in groups and exposed to the LLM as callable tools. Each cap has a lifecycle (init/start/stop) and an execute function. Groups can be enabled/disabled per session.

### Key Subsystems

- **Capabilities** (`components/claw_capabilities/cap_*`): Modular plugins. IM adapters (Telegram, QQ, Feishu, WeChat), file ops, Lua scripting, MCP client/server, scheduler, web search, system control, session management, skill management, etc.
- **Lua Modules** (`components/lua_modules/lua_module_*`): Hardware bindings exposed to Lua. GPIO, I2C, display, camera, audio, LED strip, MCPWM, UART, storage, etc. Some are conditionally compiled based on board config (e.g., camera, audio codec).
- **Memory** (`claw_memory`): Session history + structured long-term memory with store/recall/update/forget. Supports async extraction of durable facts from conversations. Two modes: `full` (LLM-powered extraction) and lightweight.
- **Skills** (`claw_skill`): Dynamic skill system. Skills are loaded from `fatfs_image/skills/` at build time and activated/deactivated per session. Each skill can bring additional capability groups.
- **Board Manager**: Uses `espressif/esp_board_manager` IDF component. Each board in `application/basic_demo/boards/` has `board_info.yaml`, `board_devices.yaml`, `board_peripherals.yaml`, and `sdkconfig.defaults.board` defining hardware config.

### Application Entry

`application/basic_demo/main/app_claw.c` — `app_claw_start()` initializes all subsystems in order: paths → event router → scheduler → memory → skills → capabilities → context providers → core → outbound IM bindings.

### FatFS Image

`application/basic_demo/fatfs_image/` contains the persistent storage image flashed with firmware: router rules, scheduler config, memory files, static assets, and skill scripts.

## Code Style

- C code uses `astyle` formatting (config in `.gitlab/ci/astyle-rules.yml`).
- Pre-commit hooks enforce: copyright headers, trailing whitespace, LF line endings, astyle formatting, conventional commit messages (subject min 15 chars), and codespell.
- Commit messages follow conventional commits format with a minimum subject length of 15 characters.
- Branch names: no uppercase letters, no more than one `/` separator.

## CI

- **GitLab CI** (`.gitlab-ci.yml`): Pre-check → Build → Deploy pipeline. Builds firmware for all boards using `idf_build_apps`, merges binaries, deploys docs.
- **GitHub Actions** (`.github/workflows/build-and-deploy.yml`): Builds firmware + Astro docs site, deploys to Cloudflare Pages on master push.
