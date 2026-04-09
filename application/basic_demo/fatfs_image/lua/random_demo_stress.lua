local delay = require("delay")
local esp_heap = require("esp_heap")

local LUA_ROOT = "/fatfs/data/lua"
local caps = esp_heap.caps()

local iterations = (args and args.iterations) or 20
local pause_ms = (args and args.pause_ms) or 200
local gc_each_round = (args and args.gc_each_round) ~= false
local include_interactive = (args and args.include_interactive) == true
local summary = {
    ok = 0,
    failed = 0,
    failures = {},
}

local demos = {
    "hello.lua",
    "coroutine_demo.lua",
    "gc_demo.lua",
    "led_strip_demo.lua",
    "display_demo.lua",
    "audio_play_test_wav.lua",
    "audio_demo.lua",
    "camera_capture_demo.lua",
}

if include_interactive then
    demos[#demos + 1] = "button_demo.lua"
    demos[#demos + 1] = "button_play_test_wav.lua"
    demos[#demos + 1] = "lcd_touch_demo.lua"
    demos[#demos + 1] = "lcd_touch_paint.lua"
end

local function info(c)
    return esp_heap.get_info(c)
end

local function print_heap_snapshot(tag)
    local default_info = info(caps.DEFAULT)
    local internal_info = info(caps.INTERNAL)
    local spiram_info = info(caps.SPIRAM)

    print(string.format(
        "[random_demo_stress] heap %s default free=%d min=%d largest=%d | internal free=%d min=%d | spiram free=%d min=%d",
        tag,
        default_info.free_size,
        default_info.minimum_free_size,
        default_info.largest_free_block,
        internal_info.free_size,
        internal_info.minimum_free_size,
        spiram_info.free_size,
        spiram_info.minimum_free_size
    ))
end

local function print_task_watermarks(tag)
    local tasks = esp_heap.get_task_watermarks()

    if tasks._warning then
        print("[random_demo_stress] task watermark note: " .. tostring(tasks._warning))
    end

    table.sort(tasks, function(a, b)
        if a.stack_high_water_mark_bytes == b.stack_high_water_mark_bytes then
            return a.name < b.name
        end
        return a.stack_high_water_mark_bytes < b.stack_high_water_mark_bytes
    end)

    print("[random_demo_stress] task watermarks " .. tag)
    for i = 1, math.min(#tasks, 6) do
        local task = tasks[i]
        print(string.format(
            "[random_demo_stress]   #%d %s state=%s stack_hwm=%dB prio=%d",
            i,
            tostring(task.name),
            tostring(task.state),
            tonumber(task.stack_high_water_mark_bytes) or -1,
            tonumber(task.current_priority) or -1
        ))
    end
end

local function run_demo(name)
    local path = LUA_ROOT .. "/" .. name
    local original_print = print
    local saw_error_output = false
    local result_ok = false
    local result_reason = nil

    local function wrapped_print(...)
        local parts = {}
        for i = 1, select("#", ...) do
            parts[i] = tostring(select(i, ...))
        end
        local line = table.concat(parts, "\t")
        if string.find(line, "ERROR:", 1, true) then
            saw_error_output = true
        end
        original_print(...)
    end

    print(string.format("[random_demo_stress] running %s", name))
    print = wrapped_print
    local ok, err = xpcall(function()
        dofile(path)
    end, debug.traceback)
    print = original_print

    if ok and not saw_error_output then
        print(string.format("[random_demo_stress] result %s ok", name))
        result_ok = true
    elseif ok then
        print(string.format("[random_demo_stress] result %s failed: demo printed ERROR output", name))
        result_reason = "demo printed ERROR output"
    else
        print(string.format("[random_demo_stress] result %s failed: %s", name, tostring(err)))
        result_reason = tostring(err)
    end

    return result_ok, result_reason
end

math.randomseed((os.time() % 2147483647) + math.floor(collectgarbage("count")))

print(string.format(
    "[random_demo_stress] start iterations=%d pause_ms=%d gc_each_round=%s include_interactive=%s",
    iterations,
    pause_ms,
    tostring(gc_each_round),
    tostring(include_interactive)
))

print_heap_snapshot("initial")
print_task_watermarks("initial")

for i = 1, iterations do
    local index = math.random(1, #demos)
    local name = demos[index]

    print(string.format("[random_demo_stress] round %d/%d pick=%s", i, iterations, name))
    print_heap_snapshot("before")
    local ok, reason = run_demo(name)
    if ok then
        summary.ok = summary.ok + 1
    else
        summary.failed = summary.failed + 1
        summary.failures[#summary.failures + 1] = {
            round = i,
            name = name,
            reason = reason or "unknown failure",
        }
    end

    if gc_each_round then
        collectgarbage("collect")
    end

    print_heap_snapshot("after")
    print_task_watermarks("after round " .. tostring(i))
    if pause_ms > 0 then
        delay.delay_ms(pause_ms)
    end
end

print_heap_snapshot("final")
print_task_watermarks("final")
print(string.format(
    "[random_demo_stress] summary total=%d ok=%d failed=%d",
    iterations,
    summary.ok,
    summary.failed
))
for i = 1, #summary.failures do
    local item = summary.failures[i]
    print(string.format(
        "[random_demo_stress] summary failure round=%d demo=%s reason=%s",
        item.round,
        item.name,
        tostring(item.reason)
    ))
end
print("[random_demo_stress] done")
