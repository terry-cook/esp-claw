# Lua Audio

This skill describes how to correctly use audio when writing Lua scripts.

## How to call
- Import it with `local audio = require("audio")`
- Call `audio.new_input(codec_dev_handle, sample_rate, channels, bits_per_sample [, gain_db])` to create an input handle
- Call `audio.new_output(codec_dev_handle, sample_rate, channels, bits_per_sample [, volume])` to create an output handle
- Call `audio.play_wav(output_handle, path)` to play a WAV file under `/fatfs/data/`
- Call `audio.record_wav(input_handle, path, duration_ms)` to record audio to a WAV file under `/fatfs/data/`
- Call `audio.loopback(input_handle, output_handle [, duration_ms])` to route input to output for monitoring
- Call `audio.set_volume(output_handle, pct)`, `audio.get_volume(output_handle)`, `audio.set_mute(output_handle, enabled)`, or `audio.set_gain(input_handle, db)` to adjust levels
- Call `audio.mic_read_level(input_handle [, duration_ms])` to read microphone level statistics such as `rms` and `peak`
- Call `audio.close(handle)` when a created handle is no longer needed

## Example
```lua
local audio = require("audio")
local bm = require("board_manager")

local output_codec, rate, channels, bits =
    bm.get_audio_codec_output_params("audio_dac")
local output = audio.new_output(output_codec, rate, channels, bits)

audio.set_volume(output, 60)
audio.play_wav(output, "/fatfs/data/test.wav")
audio.close(output)
```
