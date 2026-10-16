#include <mysql/mysql.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "proc_sampler.h"

static const int    SAMPLE_SECONDS = 5;
static const size_t MAX_QUEUE      = 500;

struct Config { std::string host, user, pass, db, emp; };
static Config g_cfg;
static int g_session_id = 0;

static std::string env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v ? v : def;
}

/* MYSQL connections are NOT thread-safe: every thread gets its own. */
static MYSQL *db_connect()
{
    MYSQL *c = mysql_init(nullptr);
    if (!c) return nullptr;
    if (!mysql_real_connect(c, g_cfg.host.c_str(), g_cfg.user.c_str(),
                            g_cfg.pass.c_str(), g_cfg.db.c_str(), 3306,
                            nullptr, 0)) {
        fprintf(stderr, "connect failed: %s\n", mysql_error(c));
        mysql_close(c);
        return nullptr;
    }
    mysql_autocommit(c, 0);              /* we commit in batches */
    return c;
}

static std::string esc(MYSQL *c, const std::string &s)
{
    std::string out(2 * s.size() + 1, '\0');
    unsigned long n = mysql_real_escape_string(c, &out[0], s.c_str(), s.size());
    out.resize(n);
    return out;
}

/* ---------- shared state between threads ---------- */
static volatile sig_atomic_t g_stop = 0;           /* set by signal handler */
static void on_signal(int) { g_stop = 1; }

struct Snapshot { int pid; std::string name; double cpu; double rss_mb; };

static std::queue<Snapshot> g_q;                   /* the bounded buffer */
static std::mutex g_mtx;                           /* protects g_q and g_done */
static std::condition_variable g_cv_not_empty;     /* consumer waits on this */
static std::condition_variable g_cv_not_full;      /* producer waits on this */
static std::condition_variable g_cv_stop;          /* sampler's interruptible sleep */
static bool g_done = false;                        /* guarded by g_mtx */

/* ---------- producer: sample /proc every SAMPLE_SECONDS ---------- */
static void sampler_thread()
{
    const double hz = (double)sysconf(_SC_CLK_TCK);
    std::map<int, Sample> prev = scan_all();
    double prev_t = now_sec();

    for (;;) {
        {   /* sleep, but wake immediately if we are told to stop */
            std::unique_lock<std::mutex> lk(g_mtx);
            if (g_cv_stop.wait_for(lk, std::chrono::seconds(SAMPLE_SECONDS),
                                   [] { return g_done; }))
                return;
        }

        std::map<int, Sample> cur = scan_all();
        double t = now_sec();
        double elapsed = t - prev_t;

        for (const auto &kv : cur) {
            auto it = prev.find(kv.first);
            if (it == prev.end()) continue;
            if (kv.second.ticks < it->second.ticks) continue;

            Snapshot s;
            s.pid    = kv.first;
            s.name   = kv.second.name;
            s.cpu    = (kv.second.ticks - it->second.ticks) / (hz * elapsed) * 100.0;
            s.rss_mb = kv.second.rss_mb;

            /* critical section: wait while the queue is full, then push */
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv_not_full.wait(lk, [] { return g_q.size() < MAX_QUEUE || g_done; });
            if (g_done) return;
            g_q.push(std::move(s));
            lk.unlock();
            g_cv_not_empty.notify_one();
        }
        prev = cur;
        prev_t = t;
    }
}

/* ---------- consumer: write snapshots to MySQL ---------- */
static void writer_thread()
{
    mysql_thread_init();
    MYSQL *conn = db_connect();
    if (!conn) {
        mysql_thread_end();
        kill(getpid(), SIGTERM);         /* ask the whole program to stop */
        return;
    }

    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    const char *sql =
        "INSERT INTO process_snapshots "
        "(session_id, process_name, pid, cpu_usage, memory_usage_mb, is_idle) "
        "VALUES (?, ?, ?, ?, ?, 0)";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        fprintf(stderr, "snapshot prepare failed: %s\n", mysql_stmt_error(stmt));
        mysql_close(conn);
        mysql_thread_end();
        kill(getpid(), SIGTERM);
        return;
    }

    int p_sid = g_session_id, p_pid = 0;
    double p_cpu = 0, p_rss = 0;
    char p_name[256] = "";
    unsigned long name_len = 0;

    MYSQL_BIND b[5];
    memset(b, 0, sizeof b);
    b[0].buffer_type = MYSQL_TYPE_LONG;
    b[0].buffer = &p_sid;
    b[1].buffer_type = MYSQL_TYPE_STRING;
    b[1].buffer = p_name;  b[1].length = &name_len;
    b[2].buffer_type = MYSQL_TYPE_LONG;
    b[2].buffer = &p_pid;
    b[3].buffer_type = MYSQL_TYPE_DOUBLE;
    b[3].buffer = &p_cpu;
    b[4].buffer_type = MYSQL_TYPE_DOUBLE;
    b[4].buffer = &p_rss;
    mysql_stmt_bind_param(stmt, b);

    long long written = 0;
    for (;;) {
        Snapshot s;
        bool drained;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv_not_empty.wait(lk, [] { return !g_q.empty() || g_done; });
            if (g_q.empty()) break;      /* told to stop AND nothing left */
            s = std::move(g_q.front());
            g_q.pop();
            drained = g_q.empty();
        }
        g_cv_not_full.notify_one();

        p_pid = s.pid;
        p_cpu = s.cpu;
        p_rss = s.rss_mb;
        strncpy(p_name, s.name.c_str(), sizeof p_name - 1);
        p_name[sizeof p_name - 1] = '\0';
        name_len = strlen(p_name);

        if (mysql_stmt_execute(stmt))
            fprintf(stderr, "snapshot insert failed: %s\n", mysql_stmt_error(stmt));
        else
            written++;

        if (drained) mysql_commit(conn); /* commit a batch when we catch up */
    }

    mysql_commit(conn);
    mysql_stmt_close(stmt);
    mysql_close(conn);
    mysql_thread_end();
    printf("writer: %lld snapshots stored\n", written);
}

/* ---------- main thread: kernel events -> process_events ---------- */
static long long read_events(MYSQL *conn)
{
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    const char *sql =
        "INSERT INTO process_events "
        "(session_id, event_type, pid, ppid, process_name, event_time) "
        "VALUES (?, ?, ?, ?, ?, FROM_UNIXTIME(? / 1000000))";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        fprintf(stderr, "event prepare failed: %s\n", mysql_stmt_error(stmt));
        return -1;
    }

    int p_sid = g_session_id, p_pid = 0, p_ppid = 0;
    long long p_us = 0;
    char p_type[16] = "", p_name[256] = "";
    unsigned long type_len = 0, name_len = 0;

    MYSQL_BIND bind[6];
    memset(bind, 0, sizeof bind);
    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = &p_sid;
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = p_type;  bind[1].length = &type_len;
    bind[2].buffer_type = MYSQL_TYPE_LONG;
    bind[2].buffer = &p_pid;
    bind[3].buffer_type = MYSQL_TYPE_LONG;
    bind[3].buffer = &p_ppid;
    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = p_name;  bind[4].length = &name_len;
    bind[5].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[5].buffer = &p_us;
    mysql_stmt_bind_param(stmt, bind);

    int fd = open("/dev/activity_logger", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/activity_logger (module loaded? run with sudo)");
        mysql_stmt_close(stmt);
        return -1;
    }

    std::string pending;
    char buf[4096];
    long long stored = 0;

    while (!g_stop) {
        ssize_t n = read(fd, buf, sizeof buf);       /* blocks until data */
        if (n < 0) {
            if (errno == EINTR) continue;            /* signal: re-check g_stop */
            perror("read");
            break;
        }
        if (n == 0) break;
        pending.append(buf, n);

        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);

            if (sscanf(line.c_str(), "%15s %d %d %lld %255[^\n]",
                       p_type, &p_pid, &p_ppid, &p_us, p_name) != 5)
                continue;
            if (p_pid == getpid())
                continue;

            type_len = strlen(p_type);
            name_len = strlen(p_name);
            if (mysql_stmt_execute(stmt)) {
                fprintf(stderr, "event insert failed: %s\n", mysql_stmt_error(stmt));
                continue;
            }
            stored++;
        }
        mysql_commit(conn);
    }

    close(fd);
    mysql_stmt_close(stmt);
    return stored;
}

int main()
{
    g_cfg.host = env_or("ACT_DB_HOST", "localhost");
    g_cfg.user = env_or("ACT_DB_USER", "monitor_user");
    g_cfg.pass = env_or("ACT_DB_PASS", "");
    g_cfg.db   = env_or("ACT_DB_NAME", "activity_monitor");
    g_cfg.emp  = env_or("ACT_EMPLOYEE", "EMP001");

    mysql_library_init(0, nullptr, nullptr);

    /* Install the handler (no SA_RESTART, so read() gets EINTR) ... */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    /* ... and block those signals BEFORE creating threads. Threads inherit
     * the block, so only the main thread (which unblocks them below) can be
     * interrupted. Otherwise Ctrl+C might hit a worker and leave main's
     * blocking read() asleep forever. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    MYSQL *conn = db_connect();
    if (!conn) return 1;

    char hostname[100];
    gethostname(hostname, sizeof hostname);
    hostname[sizeof hostname - 1] = '\0';
    std::string q = "INSERT INTO sessions (employee_id, machine_name) VALUES ('"
                  + esc(conn, g_cfg.emp) + "', '" + esc(conn, hostname) + "')";
    if (mysql_query(conn, q.c_str())) {
        fprintf(stderr, "session insert failed: %s\n", mysql_error(conn));
        return 1;
    }
    g_session_id = (int)mysql_insert_id(conn);       /* read before COMMIT */
    mysql_commit(conn);

    std::thread sampler(sampler_thread);
    std::thread writer(writer_thread);

    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);     /* main handles signals */

    printf("session %d started (Ctrl+C to stop)\n", g_session_id);
    long long events = read_events(conn);

    /* Shut down: set the flag under the lock, wake everyone, wait for them */
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_done = true;
    }
    g_cv_stop.notify_all();
    g_cv_not_full.notify_all();
    g_cv_not_empty.notify_all();
    sampler.join();
    writer.join();

    std::string e = "UPDATE sessions SET end_time = NOW() WHERE session_id = "
                  + std::to_string(g_session_id);
    mysql_query(conn, e.c_str());
    mysql_commit(conn);
    mysql_close(conn);
    mysql_library_end();
    printf("session %d closed, %lld events stored\n", g_session_id, events);
    return 0;
}
