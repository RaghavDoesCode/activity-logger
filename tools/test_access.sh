#!/usr/bin/env bash
# Proves the access rules: enrols two throwaway devices, tries to break the
# rules, prints PASS/FAIL for each check, then removes everything it created.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="${MYSQL_CMD:-sudo mysql}"
DB=activity_monitor
pass=0; fail=0
check() {   # check "label" expected(ok|fail) command...
    local label="$1" want="$2"; shift 2
    if "$@" >/dev/null 2>&1; then got=ok; else got=fail; fi
    if [ "$got" = "$want" ]; then echo "PASS  $label"; pass=$((pass+1))
    else echo "FAIL  $label (expected $want)"; fail=$((fail+1)); fi
}
val() { $ROOT -N -B "$DB" -e "$1"; }

A=$(./tools/add_device.sh TEST_A "Test Alpha" test-a); B=$(./tools/add_device.sh TEST_B "Test Beta" test-b)
AU=$(grep ^DB_USER <<<"$A" | cut -d= -f2); AP=$(grep ^DB_PASS <<<"$A" | cut -d= -f2)
BU=$(grep ^DB_USER <<<"$B" | cut -d= -f2); BP=$(grep ^DB_PASS <<<"$B" | cut -d= -f2)
a() { MYSQL_PWD="$AP" mysql -h127.0.0.1 -u"$AU" "$DB" -e "$1"; }
b() { MYSQL_PWD="$BP" mysql -h127.0.0.1 -u"$BU" "$DB" -e "$1"; }

SA=$(MYSQL_PWD="$AP" mysql -h127.0.0.1 -u"$AU" -N -B "$DB" -e "CALL start_session('test-a')")
SB=$(MYSQL_PWD="$BP" mysql -h127.0.0.1 -u"$BU" -N -B "$DB" -e "CALL start_session('test-b')")
EV="INSERT INTO process_events (session_id,event_type,pid,ppid,process_name,event_time) VALUES (%s,'EXEC',1,1,'t',NOW(6))"

check "A starts a session and gets its own employee"  ok   test "$(val "SELECT employee_id FROM sessions WHERE session_id=$SA")" = "$(val "SELECT employee_id FROM employees WHERE employee_code='TEST_A'")"
check "A inserts into its own session"                ok   a "$(printf "$EV" "$SA")"
check "B cannot insert into A's session"              fail b "$(printf "$EV" "$SA")"
check "B cannot read any table"                       fail b "SELECT * FROM employees"
check "B cannot close A's session"                    ok   b "CALL end_session($SA)"
check "  ...and A's session is still open"            ok   test "$(val "SELECT end_time IS NULL FROM sessions WHERE session_id=$SA")" = "1"
check "A closes its own session"                      ok   a "CALL end_session($SA)"
check "A cannot insert after closing"                 fail a "$(printf "$EV" "$SA")"
$ROOT "$DB" -e "UPDATE devices SET is_active=0 WHERE device_name='test-b'"
check "disabled device cannot start a session"        fail b "CALL start_session('test-b')"

# cleanup (children first because of the foreign keys)
$ROOT "$DB" -e "DELETE FROM process_events WHERE session_id IN ($SA,$SB); DELETE FROM sessions WHERE session_id IN ($SA,$SB); DELETE FROM devices WHERE device_name IN ('test-a','test-b'); DELETE FROM employees WHERE employee_code IN ('TEST_A','TEST_B'); DROP USER '$AU'@'localhost'; DROP USER '$BU'@'localhost';"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
