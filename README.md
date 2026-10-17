# Activity Logger (Linux)

A Linux activity monitor built for an Operating Systems + DBMS project. A kernel module records every process start and exit. A multithreaded C++ program samples CPU, memory and keyboard/mouse idleness. Everything is stored in MySQL and analysed with SQL reports.

## Architecture

```mermaid
flowchart TD
    subgraph Kernel
        K[kprobes: do_exit, begin_new_exec] --> F[kfifo ring buffer]
        F --> D[/dev/activity_logger]
        P[/proc]
        I[/dev/input/event*]
    end
    subgraph Logger["Logger (C++)"]
        M[Main thread: event reader]
        S[Sampler thread]
        T[Idle thread]
        Q[Bounded queue]
        W[Writer thread]
    end
    D --> M
    P --> S
    I --> T
    T --> S
    S --> Q --> W
    subgraph MySQL
        E[(process_events)]
        N[(process_snapshots)]
        X[(sessions)]
    end
    M --> E
    W --> N
    X --- E
    X --- N
```

## Database

- `sessions`: one row per logger run (parent table)
- `process_snapshots`: CPU %, memory (MB) and idle flag for each process every 5 seconds
- `process_events`: EXEC and EXIT events from the kernel, with microsecond timestamps

Both child tables reference `sessions(session_id)` by foreign key.

## OS concepts used

Kernel module, kprobes and kretprobes, kfifo ring buffer, spinlock, wait queue, misc character device, `copy_to_user`, `/proc` filesystem, evdev input devices with `poll()`, threads, producer-consumer with mutex and condition variables, atomics, POSIX signals, graceful shutdown.

## DBMS concepts used

Normalised 1-to-many schema, foreign keys, indexes, ENUM, `DATETIME(6)`, prepared statements, transactions with batched commits, one connection per thread, window functions, correlated subqueries, joins and aggregation.

## Requirements

- Ubuntu with a Linux kernel and matching headers
- MySQL 8.x
- g++ with C++17, make

```
sudo apt install -y build-essential linux-headers-$(uname -r) libmysqlclient-dev mysql-server
```

## Setup

1. Create the database and tables:
```
   sudo mysql < sql/schema.sql
```
2. Create the database user (choose your own password):
```
   sudo mysql -e "CREATE USER 'monitor_user'@'localhost' IDENTIFIED BY 'CHOOSE_A_PASSWORD'; GRANT ALL ON activity_monitor.* TO 'monitor_user'@'localhost';"
```
3. Build the kernel module and the logger:
```
   cd kernel && make && cd ..
   cd logger && g++ -std=c++17 -Wall -pthread -o logger logger.cpp $(mysql_config --cflags --libs) && cd ..
```

## Run

```
read -s -p "DB password: " ACT_DB_PASS; export ACT_DB_PASS; echo
sudo insmod kernel/proc_watch.ko
sudo env ACT_DB_PASS="$ACT_DB_PASS" ACT_IDLE_SECONDS=10 ./logger/logger
```

Press Ctrl+C to stop. The session is closed cleanly. Unload the module afterwards:

```
sudo rmmod proc_watch
```

## Reports

```
mysql -t -u monitor_user -p activity_monitor < sql/reports.sql
```

## Project layout

```
kernel/   proc_watch.c, Makefile: the kernel module
logger/   logger.cpp, proc_sampler.h, idle_monitor.h: user-space program
sql/      schema.sql, reports.sql
```

## Limitations

- Needs root to load the module and read input devices.
- Probes attach by function name, so the module is rebuilt per kernel version.
- Idle detection is machine-wide, not per application.
- The kernel buffer drops events if it fills; the count is logged on unload.
- Tested on Ubuntu in a VirtualBox VM only.
