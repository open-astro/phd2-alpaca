# PHD2 JSON-RPC API Reference

This document describes the JSON-RPC API implemented in `src/event_server.cpp`.
It is intended for automation clients and headless integrations.

## Scope And Source Of Truth

- Source of truth: the dispatch table and handlers in `src/event_server.cpp`.
- This document covers:
  - all currently implemented RPC methods
  - parameter shapes, aliases, and validation behavior
  - runtime preconditions/state constraints
  - asynchronous event stream behavior

## Transport

- Protocol: JSON-RPC 2.0 style objects over TCP
- Framing: one JSON object per line
- Responses are `\r\n` terminated
- Events and method responses share the same socket stream
- Default event server port for instance 1: `4400`

## Request/Response Basics

Request:

```json
{"jsonrpc":"2.0","method":"get_app_state","id":1}
```

Success:

```json
{"jsonrpc":"2.0","result":"Guiding","id":1}
```

Error:

```json
{"jsonrpc":"2.0","error":{"code":-32602,"message":"expected camera param"},"id":1}
```

## Params Rules

Most handlers accept both params styles:

- object form: `{"params":{"name":value}}`
- positional array form: `{"params":[value0,value1,...]}`

For positional arrays, names map in declared handler order. Example:

- `set_lock_position` declares `(x, y, exact)`, so `[120, 240, true]` maps to those fields.

## Error Codes

Standard JSON-RPC codes:

- `-32700` parse error
- `-32600` invalid request
- `-32601` method not found
- `-32602` invalid params
- `-32603` internal error

Application/runtime errors:

- usually `code: 1` for runtime/state/device failures
- a few imaging methods also use app codes `2`/`3` for specific failures

## Global Runtime Contracts

- Selection/config changes are rejected while equipment is connected:
  - `set_selected_*` methods
  - `set_indi_server`
  - `set_alpaca_server`
  - `set_selected_alpaca_device`
  - `set_profile_setup`
- `get_connected` is strict: true means all selected active components are connected.
- If `id` is omitted (notification), the server does not send a response.

## Method Groups

### Session, Profiles, Connection

- `get_profiles`
  - result: `[{"id":int,"name":string,"selected"?:true}]`

- `get_profile`
  - result: `{"id":int,"name":string}`

- `set_profile`
  - params: `{"id":int}`

- `set_profile_by_name`
  - params: `{"name":string}`

- `create_profile`
  - params:
    - required: `name`
    - optional clone source: `copy_from` or `copy_from_id` (mutually exclusive)
    - optional `select` (bool, default true)
  - result: `{"id":int,"name":string,"selected":bool}`

- `clone_profile`
  - params:
    - source: `source_id` or `source_name`
    - destination: `dest_name`
    - optional `select` (bool, default true)
  - result: `{"id":int,"name":string,"selected":bool}`

- `rename_profile`
  - params: target selector (`id` or `name`) + `new_name`
  - precondition: no equipment connected
  - result: `{"id":int,"name":string,"selected":bool}`

- `delete_profile`
  - params:
    - target selector: `id` or `name`
    - optional `delete_dark_files` (bool, default true)
  - precondition: no equipment connected
  - result: `{"deleted_id":int,"deleted_name":string,"current_profile":{"id":int,"name":string}}`

- `get_profile_setup`
  - result fields:
    - `focal_length`
    - `pixel_size`
    - `camera_binning`
    - `software_binning`
    - `guide_speed`
    - `calibration_duration`
    - `calibration_distance`
    - `high_res_encoders`
    - `auto_restore_calibration`
    - `multistar_enabled`
    - `mass_change_threshold_enabled`

- `set_profile_setup`
  - params: object containing any subset of writable setup fields above
  - precondition: no equipment connected
  - validation:
    - `focal_length`: `0..50000`
    - `pixel_size`: `(0,100]`
    - `camera_binning`: `1..64`
    - `software_binning`: `1..MAX_SOFTWARE_BINNING`
    - `guide_speed`: `(0,10]`
    - `calibration_duration`: `50..60000`
    - `calibration_distance`: `5..200`
  - result: full setup object (same shape as `get_profile_setup`)

- `get_connected`
  - result: bool

- `set_connected`
  - params: `{"connected":true|false}`

- `shutdown`
  - requests app termination

### Guiding Lifecycle

- `get_app_state`
  - result enum: `Stopped|Selected|Calibrating|Guiding|LostLock|Paused|Looping|Unknown`

- `loop`
  - start looping exposures

- `stop_capture`
  - stop capture/guiding loop

- `guide`
  - params:
    - required `settle` object: `pixels`, `time`, `timeout`
    - optional `recalibrate` (bool)
    - optional `roi` as `[x,y,width,height]`
  - notes:
    - if `/server/guide_allow_recalibrate` is false, requested recalibration is ignored

- `dither`
  - params:
    - required `amount` (number)
    - optional `raOnly` (bool, default false)
    - required `settle` object: `pixels`, `time`, `timeout`

- `get_settling`
  - result: bool

- `get_paused`
  - result: bool

- `set_paused`
  - params:
    - `paused` (bool/int)
    - optional `type`: `"full"` for full pause, otherwise guiding pause

### Exposure And Image Flow

- `get_exposure`
  - result: requested exposure duration (ms)

- `set_exposure`
  - params: `{"exposure":int_ms}`

- `get_exposure_durations`
  - result: supported exposure list (ms)

- `capture_single_frame`
  - params:
    - optional `exposure` (`1..600000` ms)
    - optional `binning` (must be valid for camera)
    - optional `gain` (`0..100`)
    - optional `subframe` as `[x,y,width,height]`
    - optional `path` (absolute path only; must not already exist)
    - optional `save` (bool)
  - rules:
    - rejected if capture is active
    - requires connected camera
    - if `save=false`, `path` is not allowed
  - result: `0` when exposure has been started
  - completion is signaled asynchronously by `SingleFrameComplete`

- `save_image`
  - saves current guider image to temp file
  - result: `{"filename":"..."}`

- `get_star_image`
  - params: optional `size` (int, minimum `15`)
  - requires a selected star and valid current image
  - result:
    - `frame`
    - `width`
    - `height`
    - `star_pos` `[x,y]` within returned crop
    - `pixels` base64-encoded 16-bit pixel bytes

- `find_star`
  - params: optional `roi` `[x,y,width,height]`
  - result: lock position point on success

- `deselect_star`
  - clears selected lock position

- `get_lock_position`
  - result: `[x,y]` or `null`

- `set_lock_position`
  - params: `x`, `y`, optional `exact` (default true)
  - if `exact=false`, lock point snaps using star-at-position logic

- `get_use_subframes`
  - result: bool

- `get_search_region`
  - result: search region radius/setting used by guider

- `get_camera_binning`
  - requires connected camera
  - result: int

- `get_camera_frame_size`
  - requires connected camera
  - result: `[width,height]`

### Equipment Inventory, Selection, INDI, Alpaca

- `get_current_equipment`
  - result object may include: `camera`, `mount`, `aux_mount`, `AO`, `rotator`
  - each present device field has `{ "name": string, "connected": bool }`

- `get_equipment_choices`
  - result:
    - `camera`: string[]
    - `mount`: string[]
    - `aux_mount`: string[]
    - `AO`: string[]
    - `rotator`: string[]

Selection getters:

- `get_selected_mount`
- `get_selected_indi_mount_driver`
- `get_selected_camera`
- `get_selected_camera_id`
- `get_selected_indi_camera_driver`
- `get_camera_bitdepth`
- `get_selected_aux_mount`
- `get_selected_ao`
- `get_selected_rotator`

Selection setters (all blocked while equipment is connected):

- `set_selected_mount`
  - params: `{"mount":"<choice>"}`

- `set_selected_indi_mount_driver`
  - params aliases: `mount_driver` or `mount` or `driver`

- `set_selected_camera`
  - params: `{"camera":"<choice>"}`

- `set_selected_camera_id`
  - params: `{"camera_id":"<driver-camera-id>"}`

- `set_selected_indi_camera_driver`
  - params aliases: `camera_driver` or `camera` or `driver`

- `set_camera_bitdepth`
  - params: `{"bitdepth":int}`
  - range: `0..32`

- `set_selected_aux_mount`
  - params: `{"aux_mount":"<choice>"}`

- `set_selected_ao`
  - params: `{"ao":"<choice>"}`

- `set_selected_rotator`
  - params: `{"rotator":"<choice>"}`

INDI server config (blocked while equipment is connected):

- `get_indi_server`
  - result: `{"host":string,"port":int}`

- `set_indi_server`
  - params: any of `host` and/or `port`
  - host must be non-empty string, port range `1..65535`

Alpaca server config (blocked while equipment is connected):

- `get_alpaca_server`
  - result:
    - `host`
    - `port`
    - `camera_device`
    - `telescope_device`
    - `rotator_device`

- `set_alpaca_server`
  - params: any subset of:
    - `host`, `port`
    - `camera_device`, `telescope_device`, `rotator_device`
  - port range `1..65535`
  - device ids must be `>= 0`
  - when matching Alpaca devices are currently selected, menu selections are auto-updated

Alpaca discovery/query:

- `discover_alpaca_servers`
  - optional params:
    - `num_queries` or alias `queries` (`1..20`, default `2`)
    - `timeout_seconds` or alias `timeout` (`1..30`, default `2`)
  - result: array like `["192.168.1.154:6800"]`
  - returns app error if build lacks Alpaca support

- `query_alpaca_devices`
  - params:
    - optional `host` and `port` (defaults from profile)
    - optional `device_type` or alias `type`:
      - `all|camera|telescope|mount|rotator`
  - result: array of objects:
    - `device_number`
    - `device_type`
    - `device_name`
    - `display_name`
    - `display` (`"Device <n>: <name>"`)

- `get_alpaca_camera_pixelsize`
  - params:
    - optional `host`, `port`
    - optional camera selector: `device_number` or alias `device` (`>= 0`)
  - result: `{"host":string,"port":int,"device_number":int,"pixel_size":number}`
  - notes:
    - reads `PixelSizeX`, falls back to `PixelSizeY`
    - returns app error if Alpaca support is not compiled in

- `get_selected_camera_pixelsize`
  - params: none
  - result: `{"camera":string,"pixel_size":number,"source":string}`
  - notes:
    - matches desktop wizard behavior: uses selected camera/ID, connects driver if needed, and queries driver-reported pixel size
    - `source` values include `driver`, `connected_camera_device`, `connected_camera_profile`, or `profile`

- `set_selected_alpaca_device`
  - params:
    - device type via `device_type` or alias `type`:
      - `camera|telescope|mount|rotator`
    - device number via either:
      - `device_number` or alias `device` (int >= 0)
      - `display` in form `"Device <n>: ..."`
  - updates per-type Alpaca device id and applies matching current selection choice when possible

### Guiding Control, Calibration, Algorithms

- `get_calibrated`
  - result: bool

- `clear_calibration`
  - params: optional `which` = `mount|ao|both` (default both)

- `flip_calibration`
  - flips calibration data for pier-side style inversion

- `guide_pulse`
  - params:
    - `amount` (int ms; negative amount flips direction)
    - `direction` (`n|s|e|w|north|south|east|west|up|down|left|right`)
    - optional `which` (`mount|ao`, default mount)
  - restrictions:
    - target device must be connected
    - rejected while calibrating/guiding or if device busy

- `get_calibration_data`
  - params: optional `which` (`mount|ao`, default mount)
  - requires selected device connected
  - result:
    - always `calibrated`
    - when calibrated also includes `xAngle`, `xRate`, `xParity`, `yAngle`, `yRate`, `yParity`, `declination`

- `get_guide_output_enabled`
- `set_guide_output_enabled`
  - setter params: `{"enabled":bool}`

- `get_algo_param_names`
  - params: `{"axis":"ra|dec|x|y"}`
  - includes `algorithmName`

- `get_algo_param`
  - params: `{"axis":"...","name":"..."}`

- `set_algo_param`
  - params: `{"axis":"...","name":"...","value":number}`

- `get_dec_guide_mode`
- `set_dec_guide_mode`
  - setter params: `{"mode":"<valid dec mode string>"}`

- `get_pixel_scale`
  - result: number or `null` if unknown

### Lock Shift, Variable Delay, Frame Limit

- `get_lock_shift_enabled`
- `set_lock_shift_enabled`
  - setter params: `{"enabled":bool}`

- `get_lock_shift_params`
  - default result uses mount coordinates (`RA/Dec`) and units (`arcsec/hr`)
  - if params request camera axes (`{"axes":"x/y"}` or `{"axes":"camera"}`), result is converted to `X/Y` and `pixels/hr`

- `set_lock_shift_params`
  - params:
    - `rate`: `[x,y]`
    - `units`: `arcsec/hr|arc-sec/hr|pixels/hr`
    - `axes`: `RA/Dec|X/Y`

- `get_variable_delay_settings`
  - result: `{"Enabled":bool,"ShortDelaySeconds":number,"LongDelaySeconds":number}`

- `set_variable_delay_settings`
  - params (required): `Enabled`, `ShortDelaySeconds`, `LongDelaySeconds`

- `get_limit_frame`
  - result: `{"roi":null}` or `{"roi":[x,y,width,height]}`

- `set_limit_frame`
  - params: required `roi`
    - `roi = null` clears frame limit
    - `roi = [x,y,width,height]` sets frame limit
  - requires camera support for frame limiting

### Calibration Files, Darks, Defect Map, Cooler, Export

- `get_calibration_files_status`
  - result fields include:
    - `profile_id`
    - `dark_library_path`, `defect_map_path`
    - `dark_library_exists`, `defect_map_exists`
    - `dark_library_compatible`, `defect_map_compatible`
    - `dark_library_loaded`, `defect_map_loaded`
    - `auto_load_darks`, `auto_load_defect_map`
    - if camera present: `dark_count_loaded`, `dark_min_exposure_seconds_loaded`, `dark_max_exposure_seconds_loaded`

- `set_dark_library_enabled`
  - params: `{"enabled":bool}`
  - enabling requires connected camera
  - result: same object as `get_calibration_files_status`

- `set_defect_map_enabled`
  - params: `{"enabled":bool}`
  - enabling requires connected camera
  - result: same object as `get_calibration_files_status`

- `build_dark_library`
  - params:
    - `frame_count` (`1..50`, default `5`)
    - `min_exposure_ms` (`1..600000`, optional)
    - `max_exposure_ms` (`1..600000`, optional)
    - `clear_existing` (bool, default false)
    - `notes` (string, optional)
    - `load_after` (bool, default true)
  - requires connected camera and no active capture
  - result includes:
    - `profile_id`
    - `dark_library_path`
    - `frame_count`
    - `exposure_count`
    - `exposures_ms`

- `build_defect_map_darks`
  - params:
    - `exposure_ms` (`1..600000`, default `3000`)
    - `frame_count` (`1..50`, default `10`)
    - `notes` (string, optional)
    - `load_after` (bool, default true)
  - requires connected camera and no active capture
  - result includes:
    - `profile_id`
    - `defect_map_path`
    - `defect_count`
    - `exposure_ms`
    - `frame_count`

- `delete_calibration_files`
  - params (optional booleans):
    - `delete_dark_library` (default true)
    - `delete_defect_map` (default true)
  - result: same object as `get_calibration_files_status`

- `get_cooler_status`
  - requires connected camera
  - result always includes `coolerOn`, `temperature`
  - if cooler on, also includes `setpoint`, `power`

- `set_cooler_state`
  - params: `{"enabled":bool}`
  - requires connected camera with cooler support
  - when enabling, configured profile setpoint is applied

- `get_ccd_temperature`
  - requires connected camera
  - result: `{"temperature":number}`

- `export_config_settings`
  - exports configuration to default data directory as `phd2_settings.txt`
  - result: `{"filename":"..."}`

## Async Events

All events include base fields:

- `Event`
- `Timestamp`
- `Host`
- `Inst`

Event names emitted:

- `Version`
- `AppState`
- `StartCalibration`
- `Calibrating`
- `CalibrationFailed`
- `CalibrationComplete`
- `CalibrationDataFlipped`
- `LoopingExposures`
- `LoopingExposuresStopped`
- `SingleFrameComplete`
- `StarSelected`
- `StarLost`
- `StartGuiding`
- `GuidingStopped`
- `Paused`
- `Resumed`
- `GuideStep`
- `GuidingDithered`
- `LockPositionSet`
- `LockPositionLost`
- `LockPositionShiftLimitReached`
- `SettleBegin`
- `Settling`
- `SettleDone`
- `Alert`
- `GuideParamChange`
- `ConfigurationChange`

### Startup Catch-Up Events

When a client connects, server sends catch-up context including:

- `Version`
- current lock position (`LockPositionSet`) if valid
- current selected star (`StarSelected`) if valid
- calibration complete events for calibrated mounts
- one of in-progress state indicators (`StartGuiding`, `StartCalibration`, or `Paused`), when applicable
- final `AppState`

## Example Workflows

### Headless Bootstrap

1. `get_profiles`
2. `set_profile` or `set_profile_by_name`
3. `get_equipment_choices`
4. `set_selected_camera` / `set_selected_mount` / optional aux/AO/rotator
5. optional INDI/Alpaca config (`set_indi_server`, `set_alpaca_server`)
6. `set_connected` true
7. `loop` or `guide`

### Dither Between Exposures

1. call `dither` with settle object
2. watch `Settling`
3. wait for `SettleDone` success before resuming imaging exposure

### Single Frame Capture With Save

1. `capture_single_frame` with `save=true` and absolute `path`
2. watch `SingleFrameComplete`
3. inspect `Success` and `Path` in the event payload

## Complete Method Name List

`clear_calibration`, `deselect_star`, `get_exposure`, `set_exposure`, `get_exposure_durations`, `get_profiles`, `get_profile`, `set_profile`, `set_profile_by_name`, `create_profile`, `clone_profile`, `rename_profile`, `delete_profile`, `get_profile_setup`, `set_profile_setup`, `get_connected`, `set_connected`, `get_calibrated`, `get_paused`, `set_paused`, `get_lock_position`, `set_lock_position`, `loop`, `stop_capture`, `guide`, `dither`, `find_star`, `get_pixel_scale`, `get_app_state`, `flip_calibration`, `get_lock_shift_enabled`, `set_lock_shift_enabled`, `get_lock_shift_params`, `set_lock_shift_params`, `save_image`, `get_star_image`, `get_use_subframes`, `get_search_region`, `shutdown`, `get_camera_binning`, `get_camera_frame_size`, `get_current_equipment`, `get_indi_server`, `set_indi_server`, `get_alpaca_server`, `set_alpaca_server`, `discover_alpaca_servers`, `query_alpaca_devices`, `get_alpaca_camera_pixelsize`, `get_selected_camera_pixelsize`, `set_selected_alpaca_device`, `get_equipment_choices`, `get_selected_mount`, `get_selected_indi_mount_driver`, `set_selected_mount`, `set_selected_indi_mount_driver`, `get_selected_camera`, `get_selected_camera_id`, `get_selected_indi_camera_driver`, `set_selected_camera`, `set_selected_camera_id`, `set_selected_indi_camera_driver`, `get_camera_bitdepth`, `set_camera_bitdepth`, `get_selected_aux_mount`, `set_selected_aux_mount`, `get_selected_ao`, `set_selected_ao`, `get_selected_rotator`, `set_selected_rotator`, `get_guide_output_enabled`, `set_guide_output_enabled`, `get_algo_param_names`, `get_algo_param`, `set_algo_param`, `get_dec_guide_mode`, `set_dec_guide_mode`, `get_settling`, `guide_pulse`, `get_calibration_data`, `capture_single_frame`, `get_calibration_files_status`, `set_dark_library_enabled`, `set_defect_map_enabled`, `build_dark_library`, `build_defect_map_darks`, `delete_calibration_files`, `get_cooler_status`, `set_cooler_state`, `get_ccd_temperature`, `export_config_settings`, `get_variable_delay_settings`, `set_variable_delay_settings`, `get_limit_frame`, `set_limit_frame`.

## Method Spec Sheets

This section is normative for integrator behavior. Each method entry describes:

- request contract (parameter names and accepted aliases)
- preconditions and runtime state gates
- result shape
- common failure modes and error code patterns
- observable side effects/events

### A) Profile, Session, Connection

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_profiles` | none | none | array of `{id,name,selected?}` | none expected | none |
| `get_profile` | none | none | `{id,name}` | none expected | none |
| `set_profile` | `id:int` | guider context must exist | `0` | `-32602` bad `id`; `1` profile switch failure | active profile changes |
| `set_profile_by_name` | `name:string` | guider context must exist | `0` | `-32602` bad/missing name; `1` profile switch failure | active profile changes |
| `create_profile` | `name`; optional `copy_from` or `copy_from_id`; optional `select:bool` | none; if `select=true`, guider context required | `{id,name,selected}` | `-32602` invalid/mutually-exclusive source args; `1` creation/select failure | new profile persisted; may become active |
| `clone_profile` | source via `source_id` or `source_name`; `dest_name`; optional `select:bool` | none; if `select=true`, guider context required | `{id,name,selected}` | `-32602` invalid source/destination args; `1` clone/select failure | new profile persisted; may become active |
| `rename_profile` | target via `id` or `name`; `new_name` | no equipment connected | `{id,name,selected}` or `0` if unchanged | `-32602` invalid profile args/name collision; `1` cannot rename while connected or rename failure | profile name updated; title updates when active profile renamed |
| `delete_profile` | target via `id` or `name`; optional `delete_dark_files:bool` | no equipment connected | `{deleted_id,deleted_name,current_profile:{id,name}}` | `-32602` invalid selector/boolean; `1` cannot delete while connected | profile removed; if deleting current profile, runtime profile settings are reloaded |
| `get_profile_setup` | none | none | setup object with 11 fields | none expected | none |
| `set_profile_setup` | object with any subset of setup fields | no equipment connected | full setup object (post-apply) | `-32602` type/range violations; `1` blocked while connected | profile settings flushed; runtime setup reloaded |
| `get_connected` | none | none | `bool` | none expected | none |
| `set_connected` | `connected:bool` | guider context must exist | `0` | `-32602` invalid boolean; `1` connect/disconnect failure | device connection state changes; corresponding events may follow |
| `shutdown` | none | none | `0` | none in normal path | app termination requested |

### B) Guiding Lifecycle, Star Selection, Dither

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_app_state` | none | none | state string | none expected | none |
| `loop` | none | none | `0` | `1` cannot start looping | transitions toward `Looping` state |
| `stop_capture` | none | none | `0` | none expected | stops looping/guiding capture path |
| `guide` | `settle:{pixels,time,timeout}`; optional `recalibrate:bool`; optional `roi:[x,y,w,h]` | guiding must be possible (`CanGuide`) | `0` | `-32602` bad settle/roi/recalibrate type; `1` cannot guide or guide failed | guiding/calibration/settle event flow starts |
| `dither` | `amount:number`; optional `raOnly:bool`; `settle:{pixels,time,timeout}` | guiding controller available | `0` | `-32602` missing/invalid params; `1` dither failure | `GuidingDithered` + settling events |
| `get_settling` | none | none | `bool` | none expected | none |
| `get_paused` | none | guider context must exist | `bool` | `1` internal guider error | none |
| `set_paused` | `paused:bool`; optional `type:"full"` | none | `0` | `-32602` invalid param types | pause state changes; `Paused`/`Resumed` events |
| `find_star` | optional `roi:[x,y,w,h]` | guider context must exist | lock point `[x,y]` | `-32602` invalid roi; `1` cannot find star | selected star/lock changes; `StarSelected` possible |
| `deselect_star` | none | guider context must exist | `0` | `1` internal guider error | lock/star reset |
| `get_lock_position` | none | guider context must exist | `[x,y]` or `null` | `1` internal guider error | none |
| `set_lock_position` | `x:number`,`y:number`, optional `exact:bool` | guider context must exist | `0` | `-32602` invalid args; `-32600`/`-32602` invalid request shape; `-32600` method-level if cannot set lock | lock position changes; `LockPositionSet` event |

### C) Exposure, Imaging, Frame Geometry

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_exposure` | none | none | exposure ms (int) | none expected | none |
| `set_exposure` | `exposure:int` | none | `0` | `-32602` invalid/missing exposure; `1` set failure | requested exposure changes |
| `get_exposure_durations` | none | none | int[] (ms) | none expected | none |
| `capture_single_frame` | optional `exposure`,`binning`,`gain`,`subframe`,`path`,`save` | no active capture; connected camera required | `0` (capture queued) | `-32602` invalid params/path rules; `1` camera/capture state failure; `2` exposure start failure | completion via `SingleFrameComplete` event |
| `save_image` | none | guider context; current image must exist | `{filename}` | `2` no image; `3` save failure | writes temporary image file |
| `get_star_image` | optional `size:int>=15` | guider context; selected star and valid image | `{frame,width,height,star_pos,pixels}` | `-32602` invalid size; `2` no star selected | none |
| `get_use_subframes` | none | none | `bool` | none expected | none |
| `get_search_region` | none | guider context | numeric search region | `1` internal guider error | none |
| `get_camera_binning` | none | connected camera | `int` | `1` camera not connected | none |
| `get_camera_frame_size` | none | connected camera | `[width,height]` | `1` camera not connected | none |

### D) Equipment Inventory And Selection

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_current_equipment` | none | none | object with present device blocks (`camera`,`mount`,`aux_mount`,`AO`,`rotator`) | none expected | none |
| `get_equipment_choices` | none | none | object with string arrays for each device class | none expected | none |
| `get_selected_mount` | none | none | string | none expected | none |
| `set_selected_mount` | `mount:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted mount selection; may update INDI mount driver |
| `get_selected_indi_mount_driver` | none | none | string | none expected | none |
| `set_selected_indi_mount_driver` | `mount_driver` (aliases: `mount`,`driver`) | no equipment connected | `0` | `-32602` missing/invalid param; `1` blocked while connected | persisted INDI mount driver; may rewrite selected mount string |
| `get_selected_camera` | none | none | string | none expected | none |
| `set_selected_camera` | `camera:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted camera selection; may update INDI camera driver |
| `get_selected_camera_id` | none | none | camera id string | none expected | none |
| `set_selected_camera_id` | `camera_id:string` | none | `0` | `-32602` missing/invalid param | persisted id for current camera selection key |
| `get_selected_indi_camera_driver` | none | none | string | none expected | none |
| `set_selected_indi_camera_driver` | `camera_driver` (aliases: `camera`,`driver`) | no equipment connected | `0` | `-32602` missing/invalid param; `1` blocked while connected | persisted INDI camera driver; may rewrite selected camera string |
| `get_camera_bitdepth` | none | none | bitdepth int (profile value or camera fallback) | none expected | none |
| `set_camera_bitdepth` | `bitdepth:int` | none | `0` | `-32602` missing/non-int/out of `0..32` | persisted profile bitdepth |
| `get_selected_aux_mount` | none | none | string | none expected | none |
| `set_selected_aux_mount` | `aux_mount:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted aux selection; may update INDI mount driver |
| `get_selected_ao` | none | none | string | none expected | none |
| `set_selected_ao` | `ao:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted AO selection |
| `get_selected_rotator` | none | none | string | none expected | none |
| `set_selected_rotator` | `rotator:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted rotator selection; may update INDI rotator driver |

### E) INDI And Alpaca Configuration / Discovery

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_indi_server` | none | none | `{host,port}` | none expected | none |
| `set_indi_server` | any of `host:string`,`port:int` | no equipment connected | `0` | `-32602` missing both fields/invalid host/port out of `1..65535`; `1` blocked while connected | persisted INDI host/port |
| `get_alpaca_server` | none | none | `{host,port,camera_device,telescope_device,rotator_device}` | none expected | none |
| `set_alpaca_server` | any subset of `host`,`port`,`camera_device`,`telescope_device`,`rotator_device` | no equipment connected | `0` | `-32602` invalid/missing all fields/port range/device < 0; `1` blocked while connected | persisted Alpaca endpoint/device ids; may auto-adjust selected Alpaca choices |
| `discover_alpaca_servers` | optional `num_queries|queries` (`1..20`), `timeout_seconds|timeout` (`1..30`) | Alpaca support compiled in | `string[]` host:port list | `-32602` invalid numeric ranges; `1` Alpaca disabled in build | network discovery queries |
| `query_alpaca_devices` | optional `host`,`port`,`device_type|type` | Alpaca support compiled in | array of `{device_number,device_type,device_name,display_name,display}` | `-32602` invalid args/type enum; `1` server query/response failure or Alpaca disabled | HTTP query to Alpaca management endpoint |
| `get_alpaca_camera_pixelsize` | optional `host`,`port`,`device_number|device` | Alpaca support compiled in | `{host,port,device_number,pixel_size}` | `-32602` invalid host/port/device args; `1` query failure or Alpaca disabled | reads Alpaca camera `pixelsizex`/`pixelsizey` |
| `get_selected_camera_pixelsize` | none | guider context and selected camera required | `{camera,pixel_size,source}` | `1` no camera selected, camera creation/connect/query failure, or driver does not report pixel size | may connect camera driver temporarily and update profile pixel size |
| `set_selected_alpaca_device` | type via `device_type|type`; device via `device_number|device` or parseable `display` | no equipment connected | `0` | `-32602` invalid type/device/display parse; `1` blocked while connected | persists selected Alpaca device number and attempts to sync active selection string |

### F) Guiding Output, Algorithms, Calibration Data

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_guide_output_enabled` | none | mount object should exist | `bool` | `1` mount not defined | none |
| `set_guide_output_enabled` | `enabled:bool` | mount object should exist | `0` | `-32602` invalid boolean; `1` mount not defined | guide output enable state changes |
| `get_algo_param_names` | `axis:string` (`ra|dec|x|y`) | none | string[] incl. `algorithmName` | `1` invalid axis | none |
| `get_algo_param` | `axis:string`, `name:string` | none | number or string (`algorithmName`) | `1` invalid args or unknown param | none |
| `set_algo_param` | `axis:string`, `name:string`, `value:number` | none | `0` | `1` invalid args or set failure | algorithm parameter updated; graph controls refreshed |
| `get_dec_guide_mode` | none | none | mode string (`Scope::DecGuideModeStr`) | none expected | none |
| `set_dec_guide_mode` | `mode:string` | none | `0` | `1` invalid mode | DEC guide mode updated; graph controls refreshed |
| `get_calibrated` | none | none | `bool` | none expected | none |
| `clear_calibration` | optional `which:string` (`mount|ao|both`) | none | `0` | `-32602` invalid `which` | mount/AO calibration cleared |
| `flip_calibration` | none | none | `0` | `1` flip failure | calibration parity/geometry flipped; `CalibrationDataFlipped` event |
| `guide_pulse` | `amount:int`, `direction:string`, optional `which` (`mount|ao`) | target device connected; guider must not be calibrating/guiding; target not busy | `0` | `1` invalid args, invalid `which`, not connected, busy/calibrating/guiding | manual pulse scheduled |
| `get_calibration_data` | optional `which` (`mount|ao`) | selected device must be connected | `{calibrated,...}` | `1` invalid `which` or device not connected | none |

### G) Lock Shift, Delay, ROI Frame Limiting

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_lock_shift_enabled` | none | guider context must exist | `bool` | `1` internal guider error | none |
| `set_lock_shift_enabled` | `enabled:bool` | guider context must exist | `0` | `-32602` invalid boolean | lock-shift enable toggled |
| `get_lock_shift_params` | optional `axes` (`x/y` or `camera` for camera-space projection) | guider context must exist | `{enabled,rate,units,axes}` | `1` internal guider error | none |
| `set_lock_shift_params` | `rate:[x,y]`, `units`, `axes` | guider context must exist | `0` | `-32602` invalid rate/units/axes | lock-shift rate model changed |
| `get_variable_delay_settings` | none | none | `{Enabled,ShortDelaySeconds,LongDelaySeconds}` | none expected | none |
| `set_variable_delay_settings` | required `Enabled`,`ShortDelaySeconds`,`LongDelaySeconds` | none | `0` | `-32602` missing/invalid types | variable delay config changed |
| `get_limit_frame` | none | none | `{roi:null}` or `{roi:[x,y,w,h]}` | none expected | none |
| `set_limit_frame` | required `roi` (`null` or `[x,y,w,h]`) | camera must exist and support frame limiting; guider context required | `0` | `-32602` missing/invalid roi; `1` no camera/unsupported/failure | frame limiting region updated |

### H) Calibration Files, Darks, Defect Map

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_calibration_files_status` | none | none | status object (paths, exists/loaded/compatible/autoload flags, optional loaded dark stats) | none expected | none |
| `set_dark_library_enabled` | `enabled:bool` | enabling requires connected camera | status object | `-32602` invalid bool; `1` camera not connected/load/unload failure | dark library load state toggled |
| `set_defect_map_enabled` | `enabled:bool` | enabling requires connected camera | status object | `-32602` invalid bool; `1` camera not connected/load/unload failure | defect map load state toggled |
| `build_dark_library` | optional `frame_count`,`min_exposure_ms`,`max_exposure_ms`,`clear_existing`,`notes`,`load_after` | connected camera; capture inactive | `{profile_id,dark_library_path,frame_count,exposure_count,exposures_ms}` | `-32602` invalid ranges/types; `1` no camera/capture active/no matched exposure/capture save failure | captures dark stacks for selected exposures, writes dark library, optional load |
| `build_defect_map_darks` | optional `exposure_ms`,`frame_count`,`notes`,`load_after` | connected camera; capture inactive | `{profile_id,defect_map_path,defect_count,exposure_ms,frame_count}` | `-32602` invalid ranges/types; `1` no camera/capture active/capture failure | captures master dark, builds defect map, saves files, optional load |
| `delete_calibration_files` | optional `delete_dark_library:bool`,`delete_defect_map:bool` | none | status object | `-32602` invalid bools | deletes selected files for current profile, clears loaded artifacts where applicable |

### I) Cooler, Temperature, Config Export

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_cooler_status` | none | connected camera | `{coolerOn,temperature}` plus `{setpoint,power}` when on | `1` camera not connected/status failure | none |
| `set_cooler_state` | `enabled:bool` | connected camera with cooler support | `0` | `-32602` invalid bool; `1` camera not connected/no cooler/set failure | toggles cooler; on enable applies configured cooler setpoint |
| `get_ccd_temperature` | none | connected camera | `{temperature}` | `1` camera not connected/read failure | none |
| `export_config_settings` | none | none | `{filename}` | `1` export failure | writes `phd2_settings.txt` to default file directory |

## Parameter Schemas

### Common Structural Types

- `roi`: `[x:int, y:int, width:int, height:int]`
- `point`: `[x:number, y:number]`
- `settle`:
  - `pixels:number`
  - `time:number` (seconds, floored to int)
  - `timeout:number` (seconds, floored to int)

### `guide` Request Schema

```json
{
  "method": "guide",
  "params": {
    "settle": {"pixels": 0.5, "time": 6, "timeout": 30},
    "recalibrate": false,
    "roi": [100, 120, 800, 600]
  },
  "id": 42
}
```

### `dither` Request Schema

```json
{
  "method": "dither",
  "params": {
    "amount": 10,
    "raOnly": false,
    "settle": {"pixels": 1.5, "time": 8, "timeout": 30}
  },
  "id": 43
}
```

### `set_lock_shift_params` Schema

```json
{
  "method": "set_lock_shift_params",
  "params": {
    "rate": [3.3, 1.1],
    "units": "arcsec/hr",
    "axes": "RA/Dec"
  },
  "id": 44
}
```

## Response Schemas For Frequently Consumed APIs

### `get_current_equipment`

```json
{
  "camera": {"name": "CameraName", "connected": true},
  "mount": {"name": "MountName", "connected": true},
  "aux_mount": {"name": "AuxMountName", "connected": false},
  "AO": {"name": "AOName", "connected": false},
  "rotator": {"name": "RotatorName", "connected": true}
}
```

### `query_alpaca_devices`

```json
[
  {
    "device_number": 0,
    "device_type": "Camera",
    "device_name": "ASI2600MM",
    "display_name": "ASI2600MM",
    "display": "Device 0: ASI2600MM"
  }
]
```

### `get_calibration_files_status`

```json
{
  "profile_id": 3,
  "dark_library_path": "/path/to/darks.fits",
  "defect_map_path": "/path/to/defectmap.fit",
  "dark_library_exists": true,
  "defect_map_exists": true,
  "dark_library_compatible": true,
  "defect_map_compatible": true,
  "dark_library_loaded": true,
  "defect_map_loaded": false,
  "auto_load_darks": true,
  "auto_load_defect_map": true,
  "dark_count_loaded": 8,
  "dark_min_exposure_seconds_loaded": 1.0,
  "dark_max_exposure_seconds_loaded": 5.0
}
```

## Embedded Web API (HTTP)

When PHD2 server mode is enabled, PHD2 also exposes a local HTTP API on `127.0.0.1` port `8080 + instance - 1`.

- `GET /` or `GET /index.html`: web portal UI
- `GET /assets/*`: static web assets
- `GET /api/setup`: consolidated setup payload used by the web wizard
- `GET /api/discover/alpaca?num_queries=&timeout_seconds=`: Alpaca UDP discovery proxy
- `POST /api/rpc`: JSON body with `{ "method": "...", "params": { ... }, "timeout_s": n }`

### `GET /api/setup` Response Schema

```json
{
  "ok": true,
  "setup": {
    "profiles": [{"id": 1, "name": "My Equipment", "selected": true}],
    "profile": {"id": 1, "name": "My Equipment"},
    "connected": false,
    "current_equipment": {
      "camera": {"name": "CameraName", "connected": false},
      "mount": {"name": "MountName", "connected": false},
      "aux_mount": {"name": "AuxMountName", "connected": false},
      "AO": {"name": "AOName", "connected": false},
      "rotator": {"name": "RotatorName", "connected": false}
    },
    "equipment_choices": {
      "camera": ["Simulator", "Alpaca Camera"],
      "mount": ["None", "Alpaca Mount"],
      "aux_mount": ["None"],
      "AO": ["None"],
      "rotator": ["None", "Alpaca Rotator"]
    },
    "selected_camera": "Alpaca Camera",
    "selected_mount": "Alpaca Mount",
    "selected_aux_mount": "None",
    "selected_ao": "None",
    "selected_rotator": "Alpaca Rotator",
    "selected_camera_id": "",
    "alpaca_server": {
      "host": "192.168.1.100",
      "port": 6800,
      "camera_device": 0,
      "telescope_device": 0,
      "rotator_device": 0
    },
    "profile_setup": {
      "focal_length": 400,
      "pixel_size": 3.76,
      "camera_binning": 1,
      "software_binning": 1,
      "guide_speed": 0.5,
      "calibration_duration": 750,
      "calibration_distance": 25,
      "high_res_encoders": false,
      "auto_restore_calibration": false,
      "multistar_enabled": true
    },
    "dark_status": {
      "profile_id": 1,
      "dark_library_path": "/path/to/darks.fits",
      "defect_map_path": "/path/to/defectmap.fit",
      "dark_library_exists": true,
      "defect_map_exists": false,
      "dark_library_compatible": true,
      "defect_map_compatible": false,
      "dark_library_loaded": true,
      "defect_map_loaded": false,
      "auto_load_darks": true,
      "auto_load_defect_map": true,
      "dark_count_loaded": 8,
      "dark_min_exposure_seconds_loaded": 1.0,
      "dark_max_exposure_seconds_loaded": 5.0
    }
  }
}
```

`/api/rpc` returns:

```json
{"ok": true, "result": ...}
```

or

```json
{"ok": false, "error": "message"}
```

Notes:
- `timeout_s` is accepted for request-shape compatibility with web clients.
- Method execution is delegated to the same internal JSON-RPC handlers used by the event server socket.

### Embedded HTTP Status & Error Semantics

- `200 OK`: successful route handling (`/`, `/assets/*`, `/api/setup`, `/api/discover/alpaca`, `/api/rpc`).
- `400 Bad Request`: malformed request body/shape (for example invalid JSON or missing required fields on `/api/rpc`).
- `404 Not Found`: unknown path or missing asset.
- `405 Method Not Allowed`: route exists but unsupported HTTP method.
- `500 Internal Server Error`: runtime/API failure while handling request.

For API endpoints, errors use:

```json
{"ok": false, "error": "message"}
```

## Error Semantics By Category

- Contract violation (`-32602`): wrong type, missing required field, out-of-range numeric value, invalid enum string.
- Request-shape violation (`-32600`): malformed request object or `params` not array/object.
- Runtime/state violation (`1`): camera/mount not connected, operation blocked by active capture or connected-equipment guard, unsupported build capability.
- Imaging pipeline specific:
  - `2`: no image / capture start issue class
  - `3`: save failure class

## Client Implementation Recommendations

- Treat response `result:0` as operation accepted/completed, depending on method semantics.
- For async operations (`capture_single_frame`, guiding/dither), use events as authoritative completion/state source.
- Read/modify/write profile-affecting settings only while disconnected to avoid state-gate failures.
- Parse device selections case-insensitively on client side but preserve exact strings returned by `get_equipment_choices` for setter calls.
- For Alpaca, prefer numeric `device_number` in `set_selected_alpaca_device` for deterministic behavior.
