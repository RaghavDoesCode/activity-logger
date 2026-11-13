"""Admin dashboard for the Linux Activity Logger.

Reads ONLY the v_* views through the read-only MySQL account `activity_admin`
(settings in ~/dashboard.conf). Login uses hashed passwords from admin_users.
"""
import os
import re
import secrets
import time
from functools import wraps

import pymysql
from flask import (Flask, abort, g, jsonify, redirect, render_template,
                   request, session, url_for)
from werkzeug.security import check_password_hash

HOME = os.path.expanduser("~")


def load_conf(path):
    conf = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                conf[k.strip()] = v.strip()
    return conf


def load_secret(path):
    """Session-signing key: created once, readable only by us (mode 0600)."""
    if not os.path.exists(path):
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_hex(32))
    with open(path) as f:
        return f.read().strip()


CONF = load_conf(os.environ.get("DASH_CONFIG", os.path.join(HOME, "dashboard.conf")))

app = Flask(__name__)
app.secret_key = load_secret(os.path.join(HOME, ".dashboard_secret"))
app.config.update(
    SESSION_COOKIE_HTTPONLY=True,      # JavaScript cannot read the cookie
    SESSION_COOKIE_SAMESITE="Lax",     # not sent on cross-site POSTs
    PERMANENT_SESSION_LIFETIME=8 * 3600,
)


# ---------------------------------------------------------------- database
def db():
    if "db" not in g:
        g.db = pymysql.connect(
            host=CONF.get("DB_HOST", "localhost"),
            unix_socket=CONF.get("DB_SOCKET", "/var/run/mysqld/mysqld.sock"),
            user=CONF["DB_USER"], password=CONF["DB_PASS"],
            database=CONF.get("DB_NAME", "activity_monitor"),
            cursorclass=pymysql.cursors.DictCursor,
            init_command="SET time_zone = '+00:00'",
            connect_timeout=5, read_timeout=15,
        )
    return g.db


@app.teardown_appcontext
def close_db(_exc):
    conn = g.pop("db", None)
    if conn is not None:
        conn.close()


def query(sql, args=()):
    with db().cursor() as cur:        # %s placeholders: values never become SQL text
        cur.execute(sql, args)
        return cur.fetchall()


# ---------------------------------------------------------------- security
FAILS = {}                              # ip -> (count, first_fail_time)
MAX_FAILS, LOCK_SECONDS = 5, 60
DUMMY_HASH = "scrypt:32768:8:1$dummy$" + "0" * 128   # equalises timing for unknown users


def locked(ip):
    count, first = FAILS.get(ip, (0, 0))
    if count >= MAX_FAILS and time.time() - first < LOCK_SECONDS:
        return True
    if time.time() - first >= LOCK_SECONDS:
        FAILS.pop(ip, None)
    return False


def csrf_token():
    if "csrf" not in session:
        session["csrf"] = secrets.token_hex(16)
    return session["csrf"]


def check_csrf():
    sent = request.form.get("csrf", "")
    if not sent or not secrets.compare_digest(sent, session.get("csrf", "")):
        abort(400)


def login_required(view):
    @wraps(view)
    def wrapped(*a, **kw):
        if "admin" not in session:
            if request.path.startswith("/api/"):
                return jsonify(error="login required"), 401
            return redirect(url_for("login"))
        return view(*a, **kw)
    return wrapped


@app.before_request
def make_nonce():
    g.nonce = secrets.token_urlsafe(16)


@app.context_processor
def inject():
    return {"nonce": g.get("nonce", ""), "csrf": csrf_token(), "admin": session.get("admin")}


@app.after_request
def headers(resp):
    n = g.get("nonce", "")
    resp.headers["Content-Security-Policy"] = (
        "default-src 'self'; "
        f"script-src 'nonce-{n}' https://cdnjs.cloudflare.com; "
        f"style-src 'nonce-{n}'; connect-src 'self'; frame-ancestors 'none'")
    resp.headers["X-Content-Type-Options"] = "nosniff"
    resp.headers["X-Frame-Options"] = "DENY"
    resp.headers["Referrer-Policy"] = "no-referrer"
    resp.headers["Cache-Control"] = "no-store"
    return resp


# ------------------------------------------------------------------ routes
@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        check_csrf()
        ip = request.remote_addr
        if locked(ip):
            error = "Too many attempts. Wait a minute."
        else:
            user = request.form.get("username", "")[:50]
            rows = query("SELECT username, pass_hash FROM admin_users "
                         "WHERE username = %s AND is_active = 1", (user,))
            ok = check_password_hash(rows[0]["pass_hash"] if rows else DUMMY_HASH,
                                     request.form.get("password", ""))
            if rows and ok:
                FAILS.pop(ip, None)
                session.clear()                 # new session after login
                session.permanent = True
                session["admin"] = rows[0]["username"]
                return redirect(url_for("overview"))
            count, first = FAILS.get(ip, (0, time.time()))
            FAILS[ip] = (count + 1, first)
            error = "Wrong username or password."
    return render_template("login.html", error=error)


@app.route("/logout", methods=["POST"])
def logout():
    check_csrf()
    session.clear()
    return redirect(url_for("login"))


@app.route("/")
@login_required
def overview():
    devices = query("SELECT employee_id, employee_code, full_name, department, "
                    "device_name, is_active, last_seen, seconds_since, is_online "
                    "FROM v_device_status ORDER BY full_name, device_name")
    week = query("SELECT employee_id, full_name, SUM(active_minutes) AS active, "
                 "SUM(idle_minutes) AS idle "
                 "FROM v_daily_activity WHERE day >= UTC_DATE() - INTERVAL 6 DAY "
                 "GROUP BY employee_id, full_name ORDER BY full_name")
    return render_template(
        "overview.html", devices=devices,
        chart={"names": [r["full_name"] for r in week],
               "active": [float(r["active"] or 0) for r in week],
               "idle": [float(r["idle"] or 0) for r in week]})


@app.route("/employee/<int:eid>")
@login_required
def employee(eid):
    devs = query("SELECT * FROM v_device_status WHERE employee_id = %s "
                 "ORDER BY device_name", (eid,))
    if not devs:
        abort(404)
    daily = query("SELECT day, active_minutes, idle_minutes, idle_pct "
                  "FROM v_daily_activity WHERE employee_id = %s "
                  "AND day >= UTC_DATE() - INTERVAL 13 DAY ORDER BY day", (eid,))
    hourly = query("SELECT hour_utc, active_samples FROM v_hourly_activity "
                   "WHERE employee_id = %s", (eid,))
    apps = query("SELECT process_name, cpu_seconds, samples_seen FROM v_top_apps "
                 "WHERE employee_id = %s ORDER BY cpu_seconds DESC LIMIT 10", (eid,))
    sessions = query("SELECT session_id, device_name, start_time, last_seen, "
                     "samples, idle_pct FROM v_session_summary "
                     "WHERE employee_id = %s ORDER BY session_id DESC LIMIT 10", (eid,))
    by_hour = {r["hour_utc"]: r["active_samples"] for r in hourly}
    return render_template(
        "employee.html", devs=devs, sessions=sessions, name=devs[0]["full_name"],
        chart={
            "days": [str(r["day"]) for r in daily],
            "active": [float(r["active_minutes"]) for r in daily],
            "idle": [float(r["idle_minutes"]) for r in daily],
            "hours": list(range(24)),
            "hour_min": [round(float(by_hour.get(h, 0)) * 5 / 60, 1) for h in range(24)],
            "apps": [r["process_name"] for r in apps],
            "cpu": [float(r["cpu_seconds"]) for r in apps],
        })


@app.route("/api/status")
@login_required
def api_status():
    rows = query("SELECT device_id, seconds_since, is_online FROM v_device_status")
    return jsonify([{"device_id": r["device_id"], "online": bool(r["is_online"]),
                     "seconds_since": r["seconds_since"]} for r in rows])


if __name__ == "__main__":
    app.run(host=os.environ.get("DASH_HOST", "0.0.0.0"),
            port=int(os.environ.get("DASH_PORT", "5000")))
