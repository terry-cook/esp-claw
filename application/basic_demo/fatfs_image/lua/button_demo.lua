local bm    = require("board_manager")
local btn   = require("button")
local delay = require("delay")

local cfg_pin = 0

local handle, herr = btn.new(cfg_pin, 0)
if not handle then
    print("[button_demo] ERROR: button.new failed: " .. tostring(herr))
    return
end
print("[button_demo] button handle created on gpio " .. tostring(cfg_pin))

-- Read initial button state
local level, lerr = btn.get_key_level(handle)
if level == nil then
    print("[button_demo] ERROR: get_key_level: " .. tostring(lerr))
else
    print("[button_demo] initial key level: " .. tostring(level))
end

-- Register callbacks for the events we care about
btn.on(handle, "press_down", function(info)
    print("[button_demo] press_down  (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
end)

btn.on(handle, "press_up", function(info)
    print("[button_demo] press_up    (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
end)

btn.on(handle, "single_click", function(info)
    print("[button_demo] single_click")
end)

btn.on(handle, "double_click", function(info)
    print("[button_demo] double_click")
end)

btn.on(handle, "long_press_start", function(info)
    print("[button_demo] long_press_start (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
end)

btn.on(handle, "long_press_up", function(info)
    print("[button_demo] long_press_up   (pressed_time=" .. tostring(info.pressed_time_ms) .. "ms)")
end)

print("[button_demo] listening for 30 seconds, press the button...")

-- Poll for events; dispatch() invokes the callbacks registered above
for _ = 1, 3000 do
    btn.dispatch()
    delay.delay_ms(10)
end

-- Clean up callbacks
btn.off(handle)
btn.close(handle)
print("[button_demo] done")
