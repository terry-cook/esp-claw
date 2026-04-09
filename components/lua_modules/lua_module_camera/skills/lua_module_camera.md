# Lua Camera

This skill describes how to correctly use camera when writing Lua scripts.

## How to call
- Import it with `local camera = require("camera")`
- Call `camera.open(dev_path)` before reading frames
- Call `camera.info()` to get stream information such as `width`, `height`, and `pixel_format`
- Call `camera.capture(save_path [, timeout_ms])` to capture a frame to a `.jpg` or `.jpeg` path under `/fatfs/data/`
- Call `camera.close()` when the camera is no longer needed

## Example
```lua
local camera = require("camera")
local board_manager = require("board_manager")

local camera_paths = board_manager.get_camera_paths()
camera.open(camera_paths.dev_path)
local info = camera.info()
print(info.width, info.height, info.pixel_format)

local frame = camera.capture("/fatfs/data/capture.jpg", 3000)
print(frame.path, frame.bytes)
camera.close()
```
