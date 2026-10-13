# SteadyState alert watcher

Always-on service that watches the hub's MQTT status/power topics and sends alerts.
Covers what the hub can't: it works when the hub's own text path fails, and it
alerts when the hub goes silent.

Alerts: COLD (< 45F, 2 readings), POWER OUT (after 20 min), POWER RESTORED
(if texted or >= 5 min), HUB OFFLINE (no live reading for 30 min) + back online.

## Config (environment variables)
| Var | Notes |
|---|---|
| MQTT_HOST, MQTT_PORT(8883), MQTT_USER, MQTT_PASS | Subscribe-only broker login (dashboard-ro or a dedicated `watcher` user) |
| MQTT_PREFIX | default `vacation/cust1/il/temp` |
| ALERT_TO | comma-separated E.164 numbers for Twilio, e.g. `+17737443630` |
| TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN, TWILIO_FROM | enables SMS |
| NTFY_TOPIC (+ NTFY_SERVER) | enables push via ntfy; use a long random topic name |
| DRY_RUN=1 | print instead of send |
| COLD_F, POWER_DELAY_MIN, POWER_RESTORE_MIN, OFFLINE_MIN, ... | thresholds, see watcher.py |

## Run / test
    pip install -r requirements.txt
    python3 test_watcher.py            # rule tests (no network)
    python3 watcher.py --test          # send one test notification
    python3 watcher.py                 # run
Start command on Railway: `python watcher.py`
