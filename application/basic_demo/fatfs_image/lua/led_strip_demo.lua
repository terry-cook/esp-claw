local ls    = require("led_strip")
local delay = require("delay")

-- Update these values to match the LED strip wiring on the target board.
local LED_GPIO_NUM = 26
local LED_COUNT = 16

-- Create the strip handle and drive all pixels together.
print("[led] creating led strip on gpio " .. tostring(LED_GPIO_NUM))
local strip, serr = ls.new(LED_GPIO_NUM, LED_COUNT)
if not strip then
    print("[led] ERROR: failed to create led strip: " .. tostring(serr))
    return
end
print("[led] led strip created")

-- Clear first so the demo always starts from a known LED state.
strip:clear()
print("[led] strip cleared")

local function fill_all(r, g, b)
    for index = 0, LED_COUNT - 1 do
        strip:set_pixel(index, r, g, b)
    end
end

-- Flash solid red twice.
for i = 1, 2 do
    print("[led] flash " .. i .. ": on")
    fill_all(255, 0, 0)
    strip:refresh()
    delay.delay_ms(300)

    print("[led] flash " .. i .. ": off")
    strip:clear()
    strip:refresh()
    delay.delay_ms(300)
end

print("[led] done")
