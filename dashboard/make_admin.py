#!/usr/bin/env python3
"""Create or reset a dashboard admin login.
The password is typed hidden, hashed here (scrypt + random salt) and only the
hash is stored. Run:  python3 dashboard/make_admin.py"""
import getpass
import re
import subprocess
import sys

from werkzeug.security import generate_password_hash

user = input("admin username: ").strip()
if not re.fullmatch(r"[A-Za-z0-9_.-]{3,50}", user):
    sys.exit("username: 3-50 letters, digits, . _ -")
pw = getpass.getpass("password (min 10 chars): ")
if len(pw) < 10 or pw != getpass.getpass("again: "):
    sys.exit("too short, or the two entries differ")

h = generate_password_hash(pw)
if not re.fullmatch(r"[A-Za-z0-9:$._=+/-]+", h):       # safe to put inside quotes
    sys.exit("unexpected hash format")
sql = ("INSERT INTO activity_monitor.admin_users (username, pass_hash) "
       f"VALUES ('{user}', '{h}') "
       "ON DUPLICATE KEY UPDATE pass_hash = VALUES(pass_hash), is_active = 1;")
subprocess.run(["sudo", "mysql"], input=sql.encode(), check=True)
print("admin saved:", user)
