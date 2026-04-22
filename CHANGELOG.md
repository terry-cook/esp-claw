# ChangeLog

## 2026-04-21

### Refactor:

* Simplified skill activation and deactivation inputs from single `skill_id` to batch-oriented `skill_ids`, and added `all=true` support for clearing active skills.
* Updated skill manager results to echo requested skills and keep session-visible capability groups in sync after batch operations.
* Updated the display skill guidance.

### Feature:

* Synced builtin Lua scripts from component `lua_scripts` directories into `basic_demo`, replacing the old scattered `fatfs_image/scripts/builtin` layout.
* Added `sync_component_lua_scripts.py` and extended CMake sync flow so builtin scripts and generated manifests can be maintained automatically.
* Added router example scripts for LLM analyze and message/file publishing, then removed the send-message and send-file examples to narrow the shipped set.

### Fix:

* Preserved reasoning content in tool-call history across `claw_core` and OpenAI-compatible LLM backend flows.
* Updated `cap_lua` skill docs to prefer reusing existing scripts before creating new ones.
* Refined capability skill documentation and tightened `cap_router_mgr` guidance to reduce ambiguous tool usage.

## 2026-04-20

### Refactor:

* Simplified `cap_lua` argument handling so `lua_run_script` and `lua_run_script_async` only forward explicit `args` payloads instead of auto-merging agent session context.
* Simplified `event_publisher.publish_message` to require a table input with explicit `source_cap`, `channel`, `chat_id`, and `text` fields.
* Removed runtime `taskYIELD` behavior from the Lua timeout hook and relaxed the hook cadence to run every 1000 instructions.
* Normalized JSON number handling in Lua runtime to always push numbers as Lua numbers instead of preserving integer form.
