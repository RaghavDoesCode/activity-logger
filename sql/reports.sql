-- Reports on the most recent session
SET @sid = (SELECT MAX(session_id) FROM sessions);

-- 1. Most frequently started programs
SELECT process_name, COUNT(*) AS starts
FROM process_events
WHERE event_type = 'EXEC' AND session_id = @sid
GROUP BY process_name
ORDER BY starts DESC
LIMIT 10;

-- 2. Longest-living processes. Several EXECs of the same pid (launcher
--    chains) are merged into one process: first start -> exit, named
--    after the last program. Events from before the session are ignored.
SELECT process_name, pid, first_started AS started, ended,
       TIMESTAMPDIFF(MICROSECOND, first_started, ended) / 1000 AS lifetime_ms
FROM (
    SELECT process_name, pid, ended,
           MIN(started) OVER (PARTITION BY pid, ended) AS first_started,
           ROW_NUMBER() OVER (PARTITION BY pid, ended ORDER BY started DESC) AS rn
    FROM (
        SELECT e.process_name, e.pid, e.event_time AS started,
               (SELECT MIN(x.event_time)
                  FROM process_events x
                 WHERE x.session_id = e.session_id
                   AND x.pid = e.pid
                   AND x.event_type = 'EXIT'
                   AND x.event_time >= e.event_time) AS ended
        FROM process_events e
        WHERE e.event_type = 'EXEC'
          AND e.session_id = @sid
          AND e.event_time >= (SELECT start_time FROM sessions
                                WHERE session_id = @sid)
    ) paired
    WHERE ended IS NOT NULL
) ranked
WHERE rn = 1
ORDER BY lifetime_ms DESC
LIMIT 10;

-- 3. Busiest parents (which process spawns the most programs)
SELECT ppid, COUNT(*) AS children_started
FROM process_events
WHERE event_type = 'EXEC' AND session_id = @sid
GROUP BY ppid
ORDER BY children_started DESC
LIMIT 10;

-- 4. Session overview: duration, number of snapshots, idle share.
--    Every sample row carries the same idle flag, so AVG(is_idle) is
--    approximately the fraction of time the user was idle.
SELECT s.session_id, s.start_time, s.end_time,
       TIMESTAMPDIFF(SECOND, s.start_time, s.end_time) AS duration_s,
       COUNT(p.snapshot_id) AS snapshots,
       ROUND(100 * AVG(p.is_idle), 1) AS idle_pct
FROM sessions s
LEFT JOIN process_snapshots p ON p.session_id = s.session_id
GROUP BY s.session_id, s.start_time, s.end_time
ORDER BY s.session_id;

-- 5. Highest CPU reading per session. ROW_NUMBER() picks exactly one row
--    per session, so ties cannot produce duplicate rows.
SELECT session_id, process_name, pid, cpu_usage, log_time
FROM (
    SELECT session_id, process_name, pid, cpu_usage, log_time,
           ROW_NUMBER() OVER (PARTITION BY session_id
                              ORDER BY cpu_usage DESC) AS rn
    FROM process_snapshots
) ranked
WHERE rn = 1
ORDER BY session_id;

-- 6. Memory trend of one program (change the name as needed). Several
--    processes can share a name, so memory is summed per sample.
SELECT DATE_FORMAT(log_time, '%H:%i:%s') AS t,
       ROUND(SUM(memory_usage_mb), 1) AS total_mb
FROM process_snapshots
WHERE session_id = @sid AND process_name = 'firefox'
GROUP BY t
ORDER BY t;
