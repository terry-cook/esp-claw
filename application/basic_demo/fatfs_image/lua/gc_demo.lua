local function kb()
    return collectgarbage("count")
end

local function print_kb(label)
    print(string.format("[gc_demo] %s: %.2f KB", label, kb()))
end

print("[gc_demo] start")
print_kb("initial")

local blocks = {}
for i = 1, 400 do
    local t = {}
    for j = 1, 64 do
        t[j] = string.format("block=%d item=%d", i, j)
    end
    blocks[i] = t
end

print_kb("after allocation")

blocks = nil
print_kb("after release refs")

local step_done = collectgarbage("step", 64)
print(string.format("[gc_demo] step result: %s", tostring(step_done)))
print_kb("after one step")

collectgarbage("collect")
print_kb("after full collect")

print("[gc_demo] done")
