-- Linux Activity Logger schema
-- sessions and process_snapshots adapted from nirmal-bhakuni/Activity_Logger

CREATE DATABASE IF NOT EXISTS activity_monitor;
USE activity_monitor;

CREATE TABLE IF NOT EXISTS sessions (
    session_id INT AUTO_INCREMENT PRIMARY KEY,
    start_time DATETIME DEFAULT CURRENT_TIMESTAMP,
    end_time DATETIME NULL,
    total_idle_seconds INT DEFAULT 0,
    employee_id VARCHAR(50),
    machine_name VARCHAR(100)
);

CREATE TABLE IF NOT EXISTS process_snapshots (
    snapshot_id INT AUTO_INCREMENT PRIMARY KEY,
    session_id INT NOT NULL,
    process_name VARCHAR(255),
    pid INT,
    cpu_usage FLOAT,
    memory_usage_mb FLOAT,
    is_idle BOOLEAN,
    log_time DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES sessions(session_id),
    INDEX idx_snapshots_session (session_id),
    INDEX idx_snapshots_name (process_name)
);

-- One row per process start/exit reported by the kernel module
CREATE TABLE IF NOT EXISTS process_events (
    event_id BIGINT AUTO_INCREMENT PRIMARY KEY,
    session_id INT NOT NULL,
    event_type ENUM('EXEC', 'EXIT') NOT NULL,
    pid INT NOT NULL,
    ppid INT,
    process_name VARCHAR(255),
    event_time DATETIME(6) NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(session_id),
    INDEX idx_events_session (session_id),
    INDEX idx_events_pid (pid)
);
