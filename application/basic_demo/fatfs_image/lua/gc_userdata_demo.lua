local btn = require("button")

local gpio_num = (args and args.gpio_num) or 0
local rounds = (args and args.rounds) or 12

print(string.format(
    "[gc_userdata_demo] start gpio=%d rounds=%d",
    gpio_num,
    rounds
))

local function make_and_drop_button(index)
    local handle, err = btn.new(gpio_num, 0)
    if not handle then
        error(string.format("button.new failed at round %d: %s", index, tostring(err)))
    end

    print(string.format("[gc_userdata_demo] round=%d created handle=%s", index, tostring(handle)))

    -- Drop the only Lua reference on purpose. The userdata __gc should release
    -- the native button handle so repeated creation does not exhaust BTN_MAX_HANDLES.
    handle = nil
    collectgarbage("collect")
    collectgarbage("collect")
end

for i = 1, rounds do
    make_and_drop_button(i)
end

local final_handle, final_err = btn.new(gpio_num, 0)
if not final_handle then
    error("[gc_userdata_demo] final create failed after GC cycling: " .. tostring(final_err))
end

print("[gc_userdata_demo] final create succeeded after repeated GC")
btn.close(final_handle)
collectgarbage("collect")
print("[gc_userdata_demo] done")
