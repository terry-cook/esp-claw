local delay = require("delay")

print("[coroutine_demo] start")

local co = coroutine.create(function(name)
    print("[coroutine_demo] worker enter: " .. tostring(name))

    for step = 1, 3 do
        print(string.format("[coroutine_demo] worker step=%d before yield", step))
        coroutine.yield("yield@" .. tostring(step))
        print(string.format("[coroutine_demo] worker step=%d after resume", step))
        delay.delay_ms(100)
    end

    return "done:" .. tostring(name)
end)

print("[coroutine_demo] initial status: " .. coroutine.status(co))

local ok, value = coroutine.resume(co, "demo-task")
print(string.format("[coroutine_demo] resume #1 ok=%s value=%s status=%s",
    tostring(ok), tostring(value), coroutine.status(co)))

ok, value = coroutine.resume(co)
print(string.format("[coroutine_demo] resume #2 ok=%s value=%s status=%s",
    tostring(ok), tostring(value), coroutine.status(co)))

ok, value = coroutine.resume(co)
print(string.format("[coroutine_demo] resume #3 ok=%s value=%s status=%s",
    tostring(ok), tostring(value), coroutine.status(co)))

ok, value = coroutine.resume(co)
print(string.format("[coroutine_demo] resume #4 ok=%s value=%s status=%s",
    tostring(ok), tostring(value), coroutine.status(co)))

local wrapped = coroutine.wrap(function()
    for i = 1, 2 do
        coroutine.yield("wrap@" .. tostring(i))
    end
    return "wrap-done"
end)

print("[coroutine_demo] wrap call #1: " .. tostring(wrapped()))
print("[coroutine_demo] wrap call #2: " .. tostring(wrapped()))
print("[coroutine_demo] wrap call #3: " .. tostring(wrapped()))

print("[coroutine_demo] done")
