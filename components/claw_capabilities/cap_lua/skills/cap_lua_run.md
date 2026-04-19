# Lua Script Execution

Use this skill when the user wants to see existing Lua scripts, run one, inspect async execution jobs, or stop them.

> **Tool-call contract**: stopping, switching, or clearing an async job only takes effect when you actually invoke `lua_stop_async_job`, `lua_stop_all_async_jobs`, or `lua_run_script_async` (with `replace:true`). The active-jobs context block is informational — it does not stop anything. Never reply "stopped / cancelled / cleared / switched" without calling the matching tool in the same turn.

> **Do NOT deactivate this skill while any async job is active.** The stop/list tools live inside this skill — dropping it leaves the running job orphaned with no way to cancel it. The platform refuses such `deactivate_skill` calls and returns a `reason` field telling you to stop the jobs first. If you see that error, call `lua_stop_all_async_jobs` (or per-id `lua_stop_async_job`), wait for the result, and only then retry `deactivate_skill` if you still need to.

## Speed

- **`lua_list_scripts`** once per task is usually enough; avoid repeated listing while iterating on the same `temp/*.lua`.
- When the agent invokes `lua_run_script_async` from an IM chat, **`args.channel` / `args.chat_id` are often injected** — you do **not** need to duplicate them in the tool JSON unless you intentionally target another chat. Omitting redundant `args` keeps the tool call smaller and faster to validate.
- Prefer **`lua_run_script_async`** only for long-running scripts; use **`lua_run_script`** for quick one-shot checks to get output in the same turn.
- To **read** an existing `.lua` source with `read_file`, use **`scripts/`** + the same relative path `lua_list_scripts` shows (e.g. `scripts/button_demo.lua`), not the bare filename.

## Rules
- Call the direct capability entrypoints, not `cap_cli`.
- Use `lua_list_scripts` to inspect files. `prefix` is optional and must also be a relative path under the Lua base directory.
- Use `lua_run_script` for short tasks that should return output immediately.
- Use `lua_run_script_async` for loops, animations, watchers, or other long-running behavior.
- Use `lua_list_async_jobs` and `lua_get_async_job` to inspect async execution state.
- Use `lua_stop_async_job` (by `job_id` or `name`) or `lua_stop_all_async_jobs` (optionally filtered by `exclusive`) to cancel running jobs.
- `path` must be a relative `.lua` path under the configured Lua base directory.
- Newly authored scripts that are still being validated must run from `temp/*.lua`.
- `args` may be an object or array. The runtime exposes it to Lua as the global `args`.
- When the tool is invoked from an IM chat session, the firmware may merge `channel`, `chat_id`, and `session_id` into `args` if those keys are absent (so Lua can send messages back to the same chat).
- `timeout_ms` is optional, but when present it must be a non-negative integer (0 = run until cancelled, only valid for async jobs).
- For async jobs, prefer setting `name` (logical handle) and `exclusive` (resource group, e.g. `"display"`) so subsequent `replace:true` calls can deterministically take over the slot.

## Minimal Examples
```json
{"path":"hello.lua"}
```

```json
{"path":"blink.lua","args":{"pin":2},"timeout_ms":3000}
```

```json
{"path":"digital_clock.lua","name":"clock","exclusive":"display","replace":true}
```

## Guidance
- If the target script does not exist yet, switch to the Lua authoring flow first.
- Prefer re-running the same relative path while iterating on a script instead of creating many near-duplicate files.
- After running a temporary script, keep using the same `temp/*.lua` path for revisions until the user confirms it should be kept.
- When saving the confirmed version, move or rewrite it under `user/` rather than keeping it in `temp/`.
