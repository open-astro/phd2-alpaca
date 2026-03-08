# PHD2 Web UI

The web UI is now embedded in the PHD2 application.

## Embedded mode (default)

1. Start PHD2.
2. Ensure `Tools -> Enable Server` is checked.
3. Open `Tools -> Open Web Portal` (or browse directly).

Default URL:

`http://127.0.0.1:8080/` for instance 1 (`8080 + instance - 1` for additional instances)

Notes:

- Runs inside PHD2 (no separate Python process required).
- Uses the same internal JSON-RPC method implementations as the event server.
- Serves static UI from packaged `webui` resources.
- Alpaca discovery endpoint: `/api/discover/alpaca`.

## Optional legacy/developer bridge

The Python bridge is kept only as a legacy/developer fallback.

- Embedded mode in PHD2 is the supported/default path.
- Use the bridge only for local troubleshooting or web UI development.
- The bridge may be removed in a future release once embedded mode is fully validated.

Legacy launch command:

```bash
python3 scripts/phd2_web_ui_server.py --listen 127.0.0.1 --http-port 8080 --phd-host 127.0.0.1 --phd-port 4400
```
