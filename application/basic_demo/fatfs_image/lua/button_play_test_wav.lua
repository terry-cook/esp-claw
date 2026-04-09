local audio = require("audio")
local bm = require("board_manager")
local btn = require("button")
local delay = require("delay")

local BUTTON_GPIO_NUM = 28
local TEST_WAV_PATH = "/fatfs/data/test.wav"
local POLL_INTERVAL_MS = 10
local RUN_TIME_MS = 60000

local output_codec, output_rate, output_channels, output_bits =
    bm.get_audio_codec_output_params("audio_dac")
if not output_codec then
    print("[button_play_test_wav] ERROR: get_audio_codec_output_params(audio_dac) failed: " .. tostring(output_rate))
    return
end

local output, out_err = audio.new_output(output_codec, output_rate, output_channels, output_bits)
if not output then
    print("[button_play_test_wav] ERROR: new_output failed: " .. tostring(out_err))
    return
end

local handle, herr = btn.new(BUTTON_GPIO_NUM, 0)
if not handle then
    print("[button_play_test_wav] ERROR: button.new failed: " .. tostring(herr))
    audio.close(output)
    return
end

local is_playing = false

btn.on(handle, "single_click", function()
    if is_playing then
        print("[button_play_test_wav] playback already in progress")
        return
    end

    is_playing = true
    print("[button_play_test_wav] playing " .. TEST_WAV_PATH .. " ...")
    audio.play_wav(output, TEST_WAV_PATH)
    print("[button_play_test_wav] playback done")
    is_playing = false
end)

print(string.format("[button_play_test_wav] button gpio=%d output=%dHz/%dch/%dbit",
      BUTTON_GPIO_NUM, output_rate, output_channels, output_bits))
print("[button_play_test_wav] press the button once to play test.wav")
print("[button_play_test_wav] running for " .. tostring(RUN_TIME_MS) .. " ms")

local ok, err = xpcall(function()
    for _ = 1, math.floor(RUN_TIME_MS / POLL_INTERVAL_MS) do
        btn.dispatch()
        delay.delay_ms(POLL_INTERVAL_MS)
    end
end, debug.traceback)

pcall(btn.off, handle)
pcall(btn.close, handle)
pcall(audio.close, output)

if not ok then
    error(err)
end

print("[button_play_test_wav] done")
