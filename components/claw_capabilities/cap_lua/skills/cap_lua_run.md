# Lua Script Execution

Use this skill when the user wants to see existing Lua scripts, run one, or inspect async execution jobs.

## Command Rule
- The LLM should call Lua through the direct capability execute entrypoints, not through `cap_cli`.
- Use `lua_list_scripts` to inspect scripts.
- Use `lua_run_script` for synchronous execution.
- Use `lua_run_script_async` for long-running or continuous scripts.
- Use `lua_list_async_jobs` and `lua_get_async_job` to inspect async jobs.

## Running a Script Synchronously
Use `lua_run_script` when the user wants immediate output.
- Required: `path`
- Optional: `args`, `timeout_ms`
- Prefer relative paths such as `hello.lua`

Examples:
```json
{
  "path": "hello.lua"
}
```

```json
{
  "path": "blink.lua",
  "args": {
    "pin": 2
  },
  "timeout_ms": 3000
}
```

If the script expects structured inputs, pass them through `args`. The runtime exposes them to Lua as the global `args`.

## Running a Script Asynchronously
Use `lua_run_script_async` for long-running or continuous scripts.
- Required: `path`
- Optional: `args`, `timeout_ms`

Examples:
```json
{
  "path": "blink.lua"
}
```

```json
{
  "path": "blink.lua",
  "args": {
    "pin": 2
  },
  "timeout_ms": 3000
}
```

After starting an async script:
- Use `lua_list_async_jobs`
- Use `lua_list_async_jobs` with `{"status":"running"}`
- Use `lua_get_async_job` with `{"job_id":"<job_id>"}`

## Execution Notes
- Paths must resolve under `/spiffs/lua` and end with `.lua`.
- `--timeout-ms` must be a positive integer when provided.
- Prefer synchronous run for short scripts that should finish and return text.
- Prefer async run for loops, animations, watchers, or long-running device behaviors.
- If the user asks to run a script that does not exist yet, switch to the Lua authoring flow first.
