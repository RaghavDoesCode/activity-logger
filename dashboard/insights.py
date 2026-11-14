"""Insights for the admin dashboard.

Two layers:
  1. findings()  - plain statistics over the v_* views. Always works, no network.
  2. ask_llm()   - optional: sends a SMALL, ANONYMISED summary (labels such as
                   "Employee A", minutes, percentages, app *categories*) to a free
                   LLM API and returns a written summary. No names, codes, device
                   names or raw process names ever leave the machine.
"""
import json
import re
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

IDLE_NOTE = ("Idle means no keyboard or mouse input for a short threshold "
             "(10 s on these devices), so reading, calls and meetings also count "
             "as idle. Treat these numbers as hints, not as a judgement of work.")
MIN_OBSERVED_MIN = 10        # below this we refuse to draw conclusions
IDLE_GAP_POINTS = 15         # idle% difference worth mentioning
TREND_POINTS = 15            # change in idle% between recent and earlier days

# ---- app categories: only these coarse labels go to the AI ----------------
SYSTEM_RE = re.compile(
    r"^(systemd|gnome-|Xorg|Xwayland|dbus|pipewire|pulseaudio|wireplumber|gsd-|ibus|"
    r"at-spi|snapd|NetworkManager|polkit|tracker|evolution-|gvfs|upowerd|colord|"
    r"packagekit|unattended|fwupd|irqbalance|cron|rsyslog|kworker|activity-logger|"
    r"mysqld|sshd|login|\(sd-pam\)|agetty|udisksd|avahi|cups|wpa_|gdm|accounts-daemon|"
    r"thermald|power-profiles|switcheroo|rtkit|bluetoothd|ModemManager|xdg-|geoclue)")
CATEGORIES = [
    ("browser", r"firefox|chrome|chromium|brave|opera|msedge|epiphany"),
    ("editor/IDE", r"^code|codium|vim|nvim|nano|gedit|emacs|pycharm|idea|clion|sublime|kate|gnome-text"),
    ("terminal/dev", r"terminal|konsole|bash|zsh|fish|python|gcc|g\+\+|cc1|make|node|npm|git|cargo|"
                     r"java|mysql|ld$|cmake"),
    ("communication", r"slack|zoom|teams|discord|telegram|thunderbird|skype|signal|whatsapp"),
    ("office/docs", r"libreoffice|soffice|evince|okular|wps|xreader"),
    ("media", r"vlc|spotify|mpv|totem|rhythmbox"),
]


def categorize(name):
    n = (name or "").lower()
    if SYSTEM_RE.match(name or ""):
        return "system"
    for cat, pat in CATEGORIES:
        if re.search(pat, n):
            return cat
    return "other"


# ---- gathering ---------------------------------------------------------------
def gather(query):
    """Read per-employee numbers for the last 7 days (UTC) from the views."""
    emps = {r["employee_id"]: {"id": r["employee_id"], "name": r["full_name"],
                               "code": r["employee_code"], "devices": 0, "online": 0,
                               "last_seen": None, "seconds_since": None,
                               "days": [], "hours": {}, "cats": {}}
            for r in query("SELECT DISTINCT employee_id, full_name, employee_code "
                           "FROM v_device_status ORDER BY employee_id")}
    for r in query("SELECT employee_id, is_online, is_active, last_seen, seconds_since "
                   "FROM v_device_status"):
        e = emps[r["employee_id"]]
        if r["is_active"]:
            e["devices"] += 1
            e["online"] += int(r["is_online"] or 0)
            if r["last_seen"] and (e["last_seen"] is None or r["last_seen"] > e["last_seen"]):
                e["last_seen"], e["seconds_since"] = r["last_seen"], r["seconds_since"]
    for r in query("SELECT employee_id, day, active_minutes, idle_minutes FROM v_daily_activity "
                   "WHERE day >= UTC_DATE() - INTERVAL 6 DAY ORDER BY day"):
        emps[r["employee_id"]]["days"].append(
            (r["day"], float(r["active_minutes"] or 0), float(r["idle_minutes"] or 0)))
    for r in query("SELECT employee_id, hour_utc, active_samples FROM v_hourly_activity"):
        emps[r["employee_id"]]["hours"][r["hour_utc"]] = float(r["active_samples"] or 0)
    for r in query("SELECT employee_id, process_name, cpu_seconds FROM v_top_apps"):
        cat = categorize(r["process_name"])
        if cat != "system":
            c = emps[r["employee_id"]]["cats"]
            c[cat] = c.get(cat, 0.0) + float(r["cpu_seconds"] or 0)

    out = []
    for i, e in enumerate(sorted(emps.values(), key=lambda x: x["id"])):
        active = sum(d[1] for d in e["days"])
        idle = sum(d[2] for d in e["days"])
        e["label"] = "Employee " + chr(ord("A") + i % 26)
        e["active"], e["idle"], e["observed"] = active, idle, active + idle
        e["idle_pct"] = round(100 * idle / (active + idle), 1) if active + idle else None
        e["enough"] = e["observed"] >= MIN_OBSERVED_MIN
        e["peak_hour"] = max(e["hours"], key=e["hours"].get) if e["hours"] else None
        tot = sum(e["cats"].values())
        mix = ({k: round(100 * v / tot) for k, v in
                sorted(e["cats"].items(), key=lambda kv: -kv[1])[:4]} if tot > 0 else {})
        e["app_mix"] = {k: v for k, v in mix.items() if v >= 1}
        e["trend"] = _trend(e["days"])
        out.append(e)
    return out


def _trend(days):
    """Idle% of the 2 newest days vs. all earlier days (None if not enough data)."""
    if len(days) < 3:
        return None
    recent, earlier = days[-2:], days[:-2]

    def pct(rows):
        a, i = sum(r[1] for r in rows), sum(r[2] for r in rows)
        return (100 * i / (a + i), a + i) if a + i else (None, 0)
    rp, rn = pct(recent)
    ep, en = pct(earlier)
    if rp is None or ep is None or rn < MIN_OBSERVED_MIN or en < MIN_OBSERVED_MIN:
        return None
    return round(rp - ep, 1)


# ---- layer 1: statistics -------------------------------------------------------
def findings(emps):
    out = []

    def add(level, title, text):
        out.append({"level": level, "title": title, "text": text})

    online = sum(1 for e in emps if e["online"])
    total_h = sum(e["observed"] for e in emps) / 60
    add("info", "Overview",
        f"{len(emps)} employee(s) enrolled, {online} online now. "
        f"{total_h:.1f} hours of activity recorded in the last 7 days (UTC).")

    ok = [e for e in emps if e["enough"]]
    for e in emps:
        if not e["enough"]:
            add("info", f"{e['name']}: not enough data",
                f"Only {e['observed']:.0f} minutes recorded in the last 7 days; "
                f"at least {MIN_OBSERVED_MIN} are needed before drawing conclusions.")

    # team comparison: each person against everyone else (weighted by time observed)
    if len(ok) >= 2:
        for e in ok:
            others = [o for o in ok if o is not e]
            oa = sum(o["active"] for o in others)
            oi = sum(o["idle"] for o in others)
            if oa + oi == 0:
                continue
            o_pct = 100 * oi / (oa + oi)
            gap = e["idle_pct"] - o_pct
            if abs(gap) >= IDLE_GAP_POINTS:
                word = "higher" if gap > 0 else "lower"
                add("watch", f"{e['name']}: idle share is {word} than the rest",
                    f"Idle {e['idle_pct']}% of the time vs {o_pct:.1f}% for the other "
                    f"{len(others)} employee(s) ({abs(gap):.0f} points {word}).")
            else:
                add("ok", f"{e['name']}: in line with the team",
                    f"Idle {e['idle_pct']}% vs {o_pct:.1f}% for the others.")
    else:
        for e in ok:
            add("info", f"{e['name']}: idle share",
                f"Idle {e['idle_pct']}% of recorded time "
                f"({e['active']:.0f} min active, {e['idle']:.0f} min idle). "
                f"Comparison needs at least two employees with enough data.")

    for e in ok:
        if e["trend"] is not None and abs(e["trend"]) >= TREND_POINTS:
            word = "up" if e["trend"] > 0 else "down"
            add("watch", f"{e['name']}: idle share trending {word}",
                f"Over the two newest days the idle share is {abs(e['trend']):.0f} points "
                f"{word} compared with the days before.")
        if e["peak_hour"] is not None:
            h = e["peak_hour"]
            add("info", f"{e['name']}: busiest hour",
                f"Most activity happens around {h:02d}:00-{(h + 1) % 24:02d}:00 UTC.")
        if e["app_mix"]:
            mix = ", ".join(f"{k} {v}%" for k, v in e["app_mix"].items())
            add("info", f"{e['name']}: what the time goes on",
                f"Share of CPU use while active: {mix}.")

    for e in emps:
        if e["devices"] and e["seconds_since"] is not None and e["seconds_since"] > 24 * 3600:
            add("watch", f"{e['name']}: no data for {e['seconds_since'] // 86400} day(s)",
                "The device has sent nothing recently. Check that it is switched on "
                "and the logger service is running.")
    return out


# ---- layer 2: the anonymised payload and the LLM call --------------------------
def payload(emps):
    ok = [e for e in emps if e["enough"]]
    a, i = sum(e["active"] for e in ok), sum(e["idle"] for e in ok)
    return {
        "period": "last 7 days, times in UTC",
        "note": "idle = no keyboard/mouse input for a short threshold",
        "team_idle_percent": round(100 * i / (a + i), 1) if a + i else None,
        "employees": [{
            "label": e["label"],
            "enough_data": e["enough"],
            "observed_minutes": round(e["observed"]),
            "active_minutes": round(e["active"]),
            "idle_percent": e["idle_pct"],
            "days_with_data": len(e["days"]),
            "busiest_hour_utc": e["peak_hour"],
            "idle_change_recent_vs_earlier_points": e["trend"],
            "app_mix_percent": e["app_mix"],
        } for e in emps],
    }


SYSTEM_PROMPT = (
    "You help an IT administrator read workstation activity statistics. Use ONLY the "
    "numbers given. Write 4 to 6 short bullet points in plain English: an overall summary, "
    "who stands out and why (refer to people only by their labels), one caution about "
    "interpreting idle time, and one concrete follow-up. Do not speculate about a person's "
    "character, health or motivation, and do not recommend discipline.")


def _post(url, headers, body, timeout=25):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json",
                                          "User-Agent": "activity-dashboard/1.0", **headers})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"AI service returned HTTP {e.code}") from None
    except (urllib.error.URLError, TimeoutError, OSError):
        raise RuntimeError("could not reach the AI service") from None
    except ValueError:
        raise RuntimeError("AI service sent an unreadable reply") from None


def _find_text(obj):
    """Tolerant extractor: Gemini's reply shape has changed between API versions."""
    if isinstance(obj, dict):
        if isinstance(obj.get("output_text"), str):
            return obj["output_text"]
        for key in ("steps", "outputs", "candidates", "content", "parts"):
            if key in obj:
                t = _find_text(obj[key])
                if t:
                    return t
        if obj.get("type") in (None, "text") and isinstance(obj.get("text"), str) \
                and not obj.get("thought"):
            return obj["text"]
    elif isinstance(obj, list):
        for item in reversed(obj):          # the final answer comes last
            t = _find_text(item)
            if t:
                return t
    return None


def ask_llm(conf, data):
    provider = conf.get("INSIGHTS_PROVIDER", "none").lower()
    prompt = "Statistics (JSON):\n" + json.dumps(data, indent=1)
    if provider == "groq" and conf.get("GROQ_API_KEY"):
        reply = _post("https://api.groq.com/openai/v1/chat/completions",
                      {"Authorization": "Bearer " + conf["GROQ_API_KEY"]},
                      {"model": conf.get("GROQ_MODEL", "llama-3.3-70b-versatile"),
                       "messages": [{"role": "system", "content": SYSTEM_PROMPT},
                                    {"role": "user", "content": prompt}],
                       "temperature": 0.3, "max_tokens": 700})
        try:
            return reply["choices"][0]["message"]["content"].strip(), "Groq"
        except (KeyError, IndexError, TypeError):
            raise RuntimeError("AI service sent an unexpected reply") from None
    if provider == "gemini" and conf.get("GEMINI_API_KEY"):
        model = conf.get("GEMINI_MODEL", "gemini-flash-latest")
        reply = _post("https://generativelanguage.googleapis.com/v1beta/models/"
                      + model + ":generateContent",
                      {"x-goog-api-key": conf["GEMINI_API_KEY"]},
                      {"systemInstruction": {"parts": [{"text": SYSTEM_PROMPT}]},
                       "contents": [{"parts": [{"text": prompt}]}],
                       "generationConfig": {"maxOutputTokens": 1500}})
        text = _find_text(reply)
        if not text:
            raise RuntimeError("AI service sent an unexpected reply")
        return text.strip(), "Gemini"
    raise RuntimeError("no AI provider is configured")


def provider_name(conf):
    p = conf.get("INSIGHTS_PROVIDER", "none").lower()
    key = {"groq": "GROQ_API_KEY", "gemini": "GEMINI_API_KEY"}.get(p)
    return p.capitalize() if key and conf.get(key) else None


def relabel(text, emps):
    """Put real names back into the AI's text (the AI only ever saw 'Employee A')."""
    names = {e["label"]: e["name"] for e in emps}
    return re.sub(r"Employee ([A-Z])\b",
                  lambda m: names.get("Employee " + m.group(1), m.group(0)), text)


def now_utc():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
