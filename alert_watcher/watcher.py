#!/usr/bin/env python3
"""
SteadyState alert watcher
=========================
Always-on MQTT subscriber. Watches the hub's status/power topics and sends
alerts (Twilio SMS and/or ntfy push) for:

  * COLD      temp below COLD_F (default 45F) on 2 consecutive live readings;
              repeats every COLD_REPEAT_H hours; ALL CLEAR at COLD_F + 2.
  * POWER     outage still ongoing after POWER_DELAY_MIN (default 20 min);
              restore message if an outage text went out or it lasted
              >= POWER_RESTORE_MIN (default 5 min).
  * HUB QUIET no live status for OFFLINE_MIN (default 30 min) -> hub offline
              (cell dropout, or power out AND battery dead); back-online msg.

All config comes from environment variables (see README.md).
"""
import json, os, ssl, sys, time, threading
from datetime import datetime
from zoneinfo import ZoneInfo

import requests

TZ = ZoneInfo(os.environ.get("TZ_NAME", "America/Chicago"))

def env_f(name, default): return float(os.environ.get(name, default))

COLD_F        = env_f("COLD_F", 45)
COLD_CLEAR_F  = COLD_F + env_f("COLD_CLEAR_HYST_F", 2)
COLD_CONFIRM  = int(env_f("COLD_CONFIRM", 2))
COLD_REPEAT_S = env_f("COLD_REPEAT_H", 6) * 3600
POWER_DELAY_S = env_f("POWER_DELAY_MIN", 20) * 60
POWER_RESTORE_S = env_f("POWER_RESTORE_MIN", 5) * 60
OFFLINE_S     = env_f("OFFLINE_MIN", 30) * 60
OFFLINE_REPEAT_S = env_f("OFFLINE_REPEAT_H", 6) * 3600


def clock(ts, with_date=False):
    d = datetime.fromtimestamp(ts, TZ)
    h = d.hour % 12 or 12
    s = f"{h}:{d.minute:02d} {'PM' if d.hour >= 12 else 'AM'}"
    return f"{d.strftime('%b %d')} {s}" if with_date else s

def same_day(a, b):
    return datetime.fromtimestamp(a, TZ).date() == datetime.fromtimestamp(b, TZ).date()

def dur(sec):
    m = max(1, int(round(sec / 60)))
    if m < 60: return f"{m} minute{'s' if m != 1 else ''}"
    if m < 1440: return f"{m // 60} hr {m % 60} min"
    return f"{m // 1440} day{'s' if m // 1440 != 1 else ''} {(m % 1440) // 60} hr"


# ───────────────────────── notifiers ─────────────────────────
class Notifier:
    """Fan-out to every configured channel. A failure in one never blocks others."""
    def __init__(self):
        self.to = [n.strip() for n in os.environ.get("ALERT_TO", "").split(",") if n.strip()]
        self.sid = os.environ.get("TWILIO_ACCOUNT_SID")
        self.tok = os.environ.get("TWILIO_AUTH_TOKEN")
        self.frm = os.environ.get("TWILIO_FROM")
        self.ntfy_topic = os.environ.get("NTFY_TOPIC")
        self.ntfy_server = os.environ.get("NTFY_SERVER", "https://ntfy.sh")
        self.dry = os.environ.get("DRY_RUN") == "1"

    def channels(self):
        c = []
        if self.sid and self.tok and self.frm and self.to: c.append("twilio")
        if self.ntfy_topic: c.append("ntfy")
        return c or (["dry-run"] if self.dry else [])

    def send(self, text, urgent=False):
        ok = False
        if self.dry:
            print(f"[NOTIFY dry-run] {text}", flush=True); ok = True
        if self.sid and self.tok and self.frm:
            for n in self.to:
                try:
                    r = requests.post(
                        f"https://api.twilio.com/2010-04-01/Accounts/{self.sid}/Messages.json",
                        auth=(self.sid, self.tok), data={"From": self.frm, "To": n, "Body": text}, timeout=20)
                    print(f"[NOTIFY twilio -> {n[-4:]}] {r.status_code}", flush=True)
                    ok = ok or r.status_code < 300
                except Exception as e:
                    print(f"[NOTIFY twilio ERROR] {e}", flush=True)
        if self.ntfy_topic:
            try:
                h = {"Title": "SteadyState"}
                if urgent: h.update({"Priority": "urgent", "Tags": "warning"})
                r = requests.post(f"{self.ntfy_server}/{self.ntfy_topic}", data=text.encode(), headers=h, timeout=20)
                print(f"[NOTIFY ntfy] {r.status_code}", flush=True)
                ok = ok or r.status_code < 300
            except Exception as e:
                print(f"[NOTIFY ntfy ERROR] {e}", flush=True)
        return ok


# ───────────────────────── rules ─────────────────────────────
class Watcher:
    def __init__(self, notifier, now):
        self.n = notifier
        self.started = now
        self.last_seen = now            # grace period: treat startup as "just heard"
        self.last_temp = None
        self.mains = "on"
        # cold
        self.cold_count = 0; self.cold_active = False; self.cold_last_sent = 0.0
        # power
        self.out_since = None; self.out_sms_sent = False
        # offline
        self.offline = False; self.offline_last_sent = 0.0
        self.lock = threading.RLock()

    def house(self):
        return f" House is {self.last_temp:.0f}F." if self.last_temp is not None else ""

    # -- inbound messages --
    def on_message(self, topic, payload, retained, now):
        with self.lock:
            try: d = json.loads(payload)
            except Exception: return
            kind = topic.rsplit("/", 1)[-1]
            if kind == "status" and d.get("mode") == "realtime" and "temp_f" in d and not retained:
                self._status(d, now)
            elif kind == "status" and d.get("event") in ("pong", "sms_result", "reboot_cmd", "ota_cmd"):
                self.last_seen = now    # any live hub message proves it's alive
            elif kind == "power" and not retained:
                self._power(d, now)
            elif kind == "power" and retained:
                # State on (re)start: only learn an ongoing outage; never alert from stale data.
                if d.get("event") == "outage" and self.out_since is None:
                    self.out_since = now; self.mains = "off"

    def _status(self, d, now):
        self.last_seen = now
        t = d.get("temp_f")
        if not isinstance(t, (int, float)): return
        self.last_temp = float(t)
        if d.get("mains") in ("on", "off"): self.mains = d["mains"]
        if self.offline:
            self.offline = False
            self.n.send(f"HUB BACK ONLINE: vacation home is reporting again.{self.house()}")
        if t < COLD_F:
            self.cold_count += 1
            if self.cold_count >= COLD_CONFIRM:
                first = not self.cold_active
                if first or now - self.cold_last_sent >= COLD_REPEAT_S:
                    pw = "is on" if self.mains == "on" else "is OUT"
                    self.n.send(f"{'COLD ALERT' if first else 'STILL COLD'}: vacation home is {t:.0f}F "
                                f"(alert below {COLD_F:.0f}F). Power {pw}.", urgent=True)
                    self.cold_active = True; self.cold_last_sent = now
        else:
            self.cold_count = 0
            if self.cold_active and t >= COLD_CLEAR_F:
                self.n.send(f"ALL CLEAR: vacation home is back to {t:.0f}F.")
                self.cold_active = False

    def _power(self, d, now):
        self.last_seen = now
        ev = d.get("event")
        if ev == "outage":
            self.mains = "off"
            if self.out_since is None: self.out_since = now
            self.out_sms_sent = False
        elif ev == "restore":
            self.mains = "on"
            secs = d.get("outage_s") or 0
            start = self.out_since if self.out_since else (now - secs if secs else None)
            if start is not None:
                length = now - start
                if self.out_sms_sent or length >= POWER_RESTORE_S:
                    wd = not same_day(start, now)
                    self.n.send(f"POWER RESTORED: power was out for {dur(length)} "
                                f"({clock(start, wd)}-{clock(now, wd)} Central).{self.house()}")
            self.out_since = None; self.out_sms_sent = False

    # -- time-driven checks (call every ~30 s) --
    def tick(self, now):
        with self.lock:
            if self.out_since and not self.out_sms_sent and now - self.out_since >= POWER_DELAY_S:
                self.n.send(f"POWER OUT: power went out at {clock(self.out_since, not same_day(self.out_since, now))} "
                            f"Central and has not been restored.{self.house()}", urgent=True)
                self.out_sms_sent = True
            quiet = now - self.last_seen
            if quiet >= OFFLINE_S and (not self.offline or now - self.offline_last_sent >= OFFLINE_REPEAT_S):
                first = not self.offline
                pw = {"on": "was on", "off": "was OUT"}.get(self.mains, "unknown")
                self.n.send(f"HUB OFFLINE: no reading from the vacation home since {clock(self.last_seen)} Central "
                            f"({dur(quiet)}).{self.house()} Power {pw} at last report.", urgent=True)
                self.offline = True; self.offline_last_sent = now


# ───────────────────────── main ──────────────────────────────
def main():
    import paho.mqtt.client as mqtt
    notifier = Notifier()
    if "--test" in sys.argv:
        print("channels:", notifier.channels())
        sys.exit(0 if notifier.send("TEST: SteadyState alert watcher can reach you.") else 1)
    host = os.environ["MQTT_HOST"]; port = int(os.environ.get("MQTT_PORT", 8883))
    prefix = os.environ.get("MQTT_PREFIX", "vacation/cust1/il/temp")
    w = Watcher(notifier, time.time())
    print(f"watcher up: channels={notifier.channels()} prefix={prefix}", flush=True)

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=os.environ.get("MQTT_CLIENT_ID", "steadystate-watcher"))
    c.username_pw_set(os.environ["MQTT_USER"], os.environ["MQTT_PASS"]); c.tls_set()
    c.on_connect = lambda cl, ud, f, rc, *a: (print("mqtt connected", rc, flush=True),
                                              cl.subscribe([(f"{prefix}/status", 0), (f"{prefix}/power", 0)]))
    c.on_message = lambda cl, ud, m: w.on_message(m.topic, m.payload, bool(m.retain), time.time())
    c.reconnect_delay_set(5, 120)
    c.connect_async(host, port, 60); c.loop_start()
    while True:
        time.sleep(30)
        try: w.tick(time.time())
        except Exception as e: print("tick error", e, flush=True)

if __name__ == "__main__":
    main()
