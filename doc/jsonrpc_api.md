# PHD2 JSON-RPC API Reference

This document describes the TCP JSON-RPC API exposed by `event_server.cpp`.
It is intended for integrators (NINA/PINS and other automation clients).

## Transport

- Protocol: JSON-RPC 2.0 style objects over TCP
- Framing: one JSON object per line (`\n` terminated)
- Request/response correlation: `id`
- Events and responses share the same socket stream
- Default event server port for instance 1: `4400`

## Request/Response Basics

Request shape:

```json
{"jsonrpc":"2.0","method":"get_app_state","id":1}
```

Success shape:

```json
{"jsonrpc":"2.0","result":"Guiding","id":1}
```

Error shape:

```json
{"jsonrpc":"2.0","error":{"code":-32602,"message":"expected camera param"},"id":1}
```

Common errors:

- `-32601`: method not found
- `-32602`: invalid params
- `-32600`: invalid request
- app-specific `code: 1`: invalid runtime state (for example selection changes while connected)

## Selection Safety Rules

- `set_selected_*` methods are rejected while equipment is connected.
- Invalid selection strings return invalid params.
- Selection values are profile-backed and persisted.

## API Method Groups

## Session/Profile/Connection

- `get_profiles`: list available profiles
- `get_profile`: current active profile
- `set_profile`: switch active profile by id
- `get_connected`: `true` when selected equipment is connected (camera + mounts + aux/AO/rotator if selected)
- `set_connected`: connect/disconnect selected equipment
- `shutdown`: request PHD2 shutdown

## Guiding Lifecycle

- `get_app_state`: guiding state (`Stopped|Selected|Calibrating|Guiding|LostLock|Paused|Looping`)
- `loop`: start looping exposures
- `stop_capture`: stop exposure/guiding loop
- `guide`: start guiding with settle/recalibrate options
- `dither`: issue dither with settle options
- `set_paused`: pause/resume guiding
- `get_paused`: query pause state
- `find_star`: auto-select star (supports ROI)
- `deselect_star`: clear selected lock position
- `clear_calibration`: clear mount calibration
- `flip_calibration`: flip calibration data for pier-side change

## Exposure / Camera Imaging

- `get_exposure`: current exposure duration
- `set_exposure`: set exposure duration
- `get_exposure_durations`: supported exposure durations
- `get_camera_frame_size`: camera frame size
- `capture_single_frame`: capture one frame (optional subframe)
- `get_star_image`: star cutout/image payload
- `save_image`: save current frame image
- `get_camera_binning`: current camera binning
- `get_ccd_temperature`: camera sensor temperature
- `get_cooler_status`: cooler status block
- `set_cooler_state`: enable/disable cooler and optional setpoint

## Guide Metrics / Control

- `get_pixel_scale`: current pixel scale
- `get_lock_position`: current lock position
- `set_lock_position`: set lock position (supports exact/current frame options)
- `get_settling`: current settling status/result
- `guide_pulse`: manual pulse guide command
- `get_guide_output_enabled`: guide output enable state
- `set_guide_output_enabled`: enable/disable guide output
- `get_dec_guide_mode`: current Dec guiding mode
- `set_dec_guide_mode`: set Dec guiding mode
- `get_calibrated`: whether calibration exists
- `get_calibration_data`: detailed calibration payload

## Algorithms / Shift Lock

- `get_algo_param_names`: list algorithm parameter names by axis
- `get_algo_param`: read algorithm parameter value
- `set_algo_param`: set algorithm parameter value
- `get_lock_shift_enabled`: lock-shift enabled state
- `set_lock_shift_enabled`: enable/disable lock-shift
- `get_lock_shift_params`: read lock-shift parameters
- `set_lock_shift_params`: set lock-shift parameters
- `get_variable_delay_settings`: read variable-delay settings
- `set_variable_delay_settings`: set variable-delay settings

## Subframes / Search Region

- `get_use_subframes`: whether subframes are enabled
- `get_search_region`: current star search region
- `get_limit_frame`: frame limit state/region
- `set_limit_frame`: set/clear frame limit region

## Equipment Inventory / Headless Selection

- `get_current_equipment`: current device names + connected state
- `get_equipment_choices`: available profile choices for `camera`, `mount`, `aux_mount`, `AO`, `rotator`

INDI server config:

- `get_indi_server`
  - result: `{"host":"<hostname>","port":<int>}`
- `set_indi_server`
  - params: `{"host":"<hostname>","port":<int>}` (`host` and `port` may be provided individually)
  - rejected while equipment is connected

Mount selection:

- `get_selected_mount`
- `set_selected_mount` with params `{"mount":"<choice>"}`
- `get_selected_indi_mount_driver`
- `set_selected_indi_mount_driver` with params `{"mount_driver":"<indiDeviceName>"}`

Camera selection:

- `get_selected_camera`
- `set_selected_camera` with params `{"camera":"<choice>"}`
- `get_selected_camera_id`
- `set_selected_camera_id` with params `{"camera_id":"<driverCameraId>"}`
- `get_selected_indi_camera_driver`
- `set_selected_indi_camera_driver` with params `{"camera_driver":"<indiDeviceName>"}`
- `get_camera_bitdepth`
- `set_camera_bitdepth` with params `{"bitdepth":<int>}`

Aux/AO/Rotator selection:

- `get_selected_aux_mount`
- `set_selected_aux_mount` with params `{"aux_mount":"<choice>"}`
- `get_selected_ao`
- `set_selected_ao` with params `{"ao":"<choice>"}`
- `get_selected_rotator`
- `set_selected_rotator` with params `{"rotator":"<choice>"}`

Alpaca discovery/config:

- `discover_alpaca_servers`
  - optional params: `{"num_queries":<int>,"timeout_seconds":<int>}`
  - result: array of server strings, e.g. `["192.168.1.154:6800"]`
- `query_alpaca_devices`
  - params: `{"host":"<hostname>","port":<int>,"device_type":"all|camera|telescope|mount|rotator"}`
  - `host`/`port` are optional (defaults from profile)
  - result: array of objects:
    - `device_number`
    - `device_type`
    - `device_name`
    - `display_name`
    - `display` (format: `Device <n>: <name>`)
- `get_alpaca_server`
  - result:
    - `host`
    - `port`
    - `camera_device`
    - `telescope_device`
    - `rotator_device`
- `set_alpaca_server`
  - params:
    - `host`, `port`
    - optional `camera_device`, `telescope_device`, `rotator_device`
  - updates profile-backed Alpaca server/device settings
  - rejected while equipment is connected
- `set_selected_alpaca_device`
  - params:
    - `{"device_type":"camera|telescope|mount|rotator","device_number":<int>}`
    - or `{"device_type":"...","display":"Device <n>: <name>"}`
  - updates per-type Alpaca device id and applies matching selection choice when available
  - rejected while equipment is connected

## Config Export

- `export_config_settings`: export configuration payload for diagnostics/migration

## Events

Client should also consume asynchronous event objects (`{"Event":"..."}`) on the same stream.
Commonly consumed events include:

- `Version`, `AppState`, `GuideStep`, `GuidingDithered`
- `Settling`, `SettleDone`
- `Paused`, `Resumed`
- `StartCalibration`, `CalibrationComplete`
- `StarSelected`, `StarLost`
- `StartGuiding`
- `LockPositionSet`, `LockPositionLost`, `LockPositionShiftLimitReached`
- `ConfigurationChange`
