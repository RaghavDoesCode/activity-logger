-- Step 6: what the admin dashboard may see.
-- The dashboard never touches the raw tables. It reads these VIEWS through a
-- read-only account (see tools/setup_admin_db.sh).
-- One SAMPLE = one distinct log_time (sampler runs every 5 s). All times UTC.
USE activity_monitor;

SET @have := (SELECT COUNT(*) FROM information_schema.statistics
               WHERE table_schema = DATABASE() AND table_name = 'process_snapshots'
                 AND index_name = 'idx_snap_sess_time');
SET @ddl := IF(@have = 0,
    'CREATE INDEX idx_snap_sess_time ON process_snapshots (session_id, log_time)',
    'SELECT 1');
PREPARE s FROM @ddl; EXECUTE s; DEALLOCATE PREPARE s;

CREATE TABLE IF NOT EXISTS admin_users (
    admin_id   INT AUTO_INCREMENT PRIMARY KEY,
    username   VARCHAR(50) NOT NULL UNIQUE,
    pass_hash  VARCHAR(255) NOT NULL,
    is_active  BOOLEAN NOT NULL DEFAULT TRUE,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE OR REPLACE VIEW v_samples AS
SELECT session_id, log_time, MAX(is_idle) AS is_idle, COUNT(*) AS processes,
       ROUND(SUM(cpu_usage), 2) AS total_cpu, ROUND(SUM(memory_usage_mb),1) AS total_mem_mb
FROM process_snapshots
GROUP BY session_id, log_time;

CREATE OR REPLACE VIEW v_session_summary AS
SELECT s.session_id, e.employee_id, e.employee_code, e.full_name, e.department,
       d.device_id, d.device_name, s.start_time, s.end_time,
       COALESCE(x.last_sample, s.end_time, s.start_time) AS last_seen,
       COALESCE(x.samples, 0) AS samples,
       COALESCE(x.idle_samples, 0) AS idle_samples,
       COALESCE(x.samples, 0) - COALESCE(x.idle_samples, 0) AS active_samples,
       ROUND(100 * COALESCE(x.idle_samples, 0) / NULLIF(x.samples, 0), 1) AS idle_pct
FROM sessions s
JOIN devices   d ON d.device_id   = s.device_id
JOIN employees e ON e.employee_id = d.employee_id
LEFT JOIN (SELECT session_id, COUNT(*) AS samples, SUM(is_idle) AS idle_samples,
                  MAX(log_time) AS last_sample
             FROM v_samples GROUP BY session_id) x ON x.session_id = s.session_id;

CREATE OR REPLACE VIEW v_device_status AS
SELECT e.employee_id, e.employee_code, e.full_name, e.department,
       d.device_id, d.device_name, d.is_active, m.last_seen,
       TIMESTAMPDIFF(SECOND, m.last_seen, UTC_TIMESTAMP()) AS seconds_since,
       CASE WHEN m.last_seen >= UTC_TIMESTAMP() - INTERVAL 90 SECOND THEN 1 ELSE 0 END AS is_online
FROM devices d
JOIN employees e ON e.employee_id = d.employee_id
LEFT JOIN (SELECT s.device_id, MAX(COALESCE(x.last_sample, s.start_time)) AS last_seen
             FROM sessions s
             LEFT JOIN (SELECT session_id, MAX(log_time) AS last_sample
                          FROM process_snapshots GROUP BY session_id) x
                    ON x.session_id = s.session_id
            GROUP BY s.device_id) m ON m.device_id = d.device_id;

CREATE OR REPLACE VIEW v_daily_activity AS
SELECT e.employee_id, e.employee_code, e.full_name, e.department,
       DATE(v.log_time) AS day, COUNT(*) AS samples,
       SUM(v.is_idle = 0) AS active_samples,
       ROUND(SUM(v.is_idle = 0) * 5 / 60, 1) AS active_minutes,
       ROUND(SUM(v.is_idle = 1) * 5 / 60, 1) AS idle_minutes,
       ROUND(100 * AVG(v.is_idle), 1) AS idle_pct
FROM v_samples v
JOIN sessions  s ON s.session_id  = v.session_id
JOIN devices   d ON d.device_id   = s.device_id
JOIN employees e ON e.employee_id = d.employee_id
GROUP BY e.employee_id, e.employee_code, e.full_name, e.department, DATE(v.log_time);

CREATE OR REPLACE VIEW v_hourly_activity AS
SELECT e.employee_id, e.employee_code, e.full_name, HOUR(v.log_time) AS hour_utc,
       COUNT(*) AS samples, SUM(v.is_idle = 0) AS active_samples
FROM v_samples v
JOIN sessions  s ON s.session_id  = v.session_id
JOIN devices   d ON d.device_id   = s.device_id
JOIN employees e ON e.employee_id = d.employee_id
GROUP BY e.employee_id, e.employee_code, e.full_name, HOUR(v.log_time);

CREATE OR REPLACE VIEW v_top_apps AS
SELECT e.employee_id, e.employee_code, e.full_name, p.process_name,
       COUNT(DISTINCT p.session_id, p.log_time) AS samples_seen,
       ROUND(SUM(p.cpu_usage) * 5 / 100, 1) AS cpu_seconds,
       ROUND(AVG(p.memory_usage_mb), 1) AS avg_mem_mb
FROM process_snapshots p
JOIN sessions  s ON s.session_id  = p.session_id
JOIN devices   d ON d.device_id   = s.device_id
JOIN employees e ON e.employee_id = d.employee_id
WHERE p.is_idle = 0
GROUP BY e.employee_id, e.employee_code, e.full_name, p.process_name;
