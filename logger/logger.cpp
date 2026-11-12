#include <mysql/mysql.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <map>
#include <queue>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "proc_sampler.h"
#include "idle_monitor.h"
#include "agent_config.h"
#include "spool.h"

/*  Data flow (an "outbox"):
 *
 *    kernel events (main thread) --+
 *                                  +--> queue --> spooler thread --> spool files --> uploader thread --> MySQL
 *    /proc sampler thread ---------+
 *
 *  The spooler only ever touches the local disk, so recording never waits for
 *  the network. The uploader is the only thread that talks to MySQL; if the
 *  server is away it keeps retrying while the spool grows. */

static const int    SAMPLE_SECONDS = 5;
static const size_t MAX_QUEUE      = 5000;

static AgentConfig g_cfg;
static fs::path    g_spool;
static std::string g_run_uuid, g_machine;
static long long   g_run_start = 0;

static std::string env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v ? v : def;
}

/* MYSQL connections are NOT thread-safe: only the uploader thread uses one. */
static MYSQL *db_connect()
{
    MYSQL *c = mysql_init(nullptr);
    if (!c) return nullptr;
    /* Encrypt, and check the server's certificate against our copy of the CA */
    enum mysql_ssl_mode mode = SSL_MODE_VERIFY_CA;
    mysql_options(c, MYSQL_OPT_SSL_MODE, &mode);
    mysql_options(c, MYSQL_OPT_SSL_CA, g_cfg.ca.c_str());
    /* A dead network must not make us wait forever */
    unsigned int connect_to = 5, rw_to = 30;
    mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &connect_to);
    mysql_options(c, MYSQL_OPT_READ_TIMEOUT, &rw_to);
    mysql_options(c, MYSQL_OPT_WRITE_TIMEOUT, &rw_to);
    /* Every device stores times in UTC, whatever the server's time zone is */
    mysql_options(c, MYSQL_INIT_COMMAND, "SET time_zone = '+00:00'");
    if (!mysql_real_connect(c, g_cfg.host.c_str(), g_cfg.user.c_str(),
                            g_cfg.pass.c_str(), g_cfg.db.c_str(), g_cfg.port,
                            nullptr, CLIENT_MULTI_RESULTS)) {
        fprintf(stderr, "connect failed: %s\n", mysql_error(c));
        mysql_close(c);
        return nullptr;
    }
    mysql_autocommit(c, 0);              /* one transaction per segment */
    return c;
}

static std::string esc(MYSQL *c, const std::string &s)
{
    std::string out(2 * s.size() + 1, '\0');
    unsigned long n = mysql_real_escape_string(c, &out[0], s.c_str(), s.size());
    out.resize(n);
    return out;
}

/* Run a CALL statement. A stored procedure can return result sets, and every
 * one must be read or the connection becomes unusable ("commands out of
 * sync"). *c0 and *c1 receive the first two columns of the first row. */
static bool call_proc(MYSQL *c, const std::string &sql, int *c0 = nullptr, int *c1 = nullptr)
{
    if (mysql_query(c, sql.c_str()))
        return false;
    bool first_set = true;
    do {
        MYSQL_RES *r = mysql_store_result(c);
        if (r) {
            MYSQL_ROW row = mysql_fetch_row(r);
            if (row && first_set) {
                if (c0 && row[0]) *c0 = atoi(row[0]);
                if (c1 && mysql_num_fields(r) > 1 && row[1]) *c1 = atoi(row[1]);
                first_set = false;
            }
            mysql_free_result(r);
        } else if (mysql_field_count(c) != 0) {
            return false;
        }
    } while (mysql_next_result(c) == 0);
    return mysql_errno(c) == 0;
}

/* ---------- shared state between threads ---------- */
static volatile sig_atomic_t g_stop = 0;           /* set by signal handler */
static void on_signal(int) { g_stop = 1; }

static std::queue<Record> g_q;                     /* the bounded buffer */
static std::mutex g_mtx;                           /* protects g_q and g_done */
static std::condition_variable g_cv_not_empty;     /* consumer waits on this */
static std::condition_variable g_cv_not_full;      /* producers wait on this */
static std::condition_variable g_cv_stop;          /* sampler's interruptible sleep */
static bool g_done = false;                        /* guarded by g_mtx */

/* uploader's own lock: wake-ups and the stop request */
static std::mutex g_up_mtx;
static std::condition_variable g_up_cv;
static bool g_up_stop = false, g_up_kick = false;

static void up_kick()
{
    { std::lock_guard<std::mutex> lk(g_up_mtx); g_up_kick = true; }
    g_up_cv.notify_one();
}

static bool up_stopping()
{
    std::lock_guard<std::mutex> lk(g_up_mtx);
    return g_up_stop;
}

/* Sleep up to `seconds`; wake early on stop (and, if asked, on new data) */
static void up_wait(int seconds, bool wake_on_kick)
{
    std::unique_lock<std::mutex> lk(g_up_mtx);
    g_up_cv.wait_for(lk, std::chrono::seconds(seconds),
                     [&] { return g_up_stop || (wake_on_kick && g_up_kick); });
    g_up_kick = false;
}

/* producer side of the bounded buffer; false means "we are shutting down" */
static bool push_record(Record &&r)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    g_cv_not_full.wait(lk, [] { return g_q.size() < MAX_QUEUE || g_done; });
    if (g_done) return false;
    g_q.push(std::move(r));
    lk.unlock();
    g_cv_not_empty.notify_one();
    return true;
}

/* ---------- producer 1: sample /proc every SAMPLE_SECONDS ---------- */
static void sampler_thread()
{
    const double hz = (double)sysconf(_SC_CLK_TCK);
    std::map<int, Sample> prev = scan_all();
    const double idle_limit = atof(env_or("ACT_IDLE_SECONDS", "60").c_str());
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
        int idle_flag = (idle_seconds() >= idle_limit) ? 1 : 0;
        if (getenv("ACT_DEBUG"))
            fprintf(stderr, "[sampler] idle for %.1f s -> flag %d\n", idle_seconds(), idle_flag);
        long long wall = (long long)time(nullptr);   /* the device's own clock */

        for (const auto &kv : cur) {
            auto it = prev.find(kv.first);
            if (it == prev.end()) continue;
            if (kv.second.ticks < it->second.ticks) continue;

            Record r;
            r.kind   = 'P';
            r.t      = wall;
            r.pid    = kv.first;
            r.name   = kv.second.name;
            r.cpu    = (kv.second.ticks - it->second.ticks) / (hz * elapsed) * 100.0;
            r.rss_mb = kv.second.rss_mb;
            r.idle   = idle_flag;
            if (!push_record(std::move(r)))
                return;
        }
        prev = cur;
        prev_t = t;
    }
}

/* ---------- consumer 1: queue -> spool files (local disk only) ---------- */
static void spooler_thread()
{
    SpoolWriter w(g_spool, g_run_uuid, g_run_start, g_machine, g_cfg.spool_max_mb);
    for (;;) {
        std::deque<Record> batch;
        bool done;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv_not_empty.wait_for(lk, std::chrono::seconds(1),
                                    [] { return !g_q.empty() || g_done; });
            while (!g_q.empty()) {
                batch.push_back(std::move(g_q.front()));
                g_q.pop();
            }
            done = g_done;        /* producers stop pushing once g_done is set */
        }
        g_cv_not_full.notify_all();

        for (const Record &r : batch)
            w.append(r);
        w.flush();
        if (w.should_rotate()) {
            w.rotate();
            up_kick();
        }
        if (done)
            break;
    }
    w.finish((long long)time(nullptr));
    up_kick();
    if (w.dropped() || w.dropped_segments())
        fprintf(stderr, "spool: %llu record(s) and %llu segment(s) were dropped\n",
                w.dropped(), w.dropped_segments());
}

/* ---------- consumer 2: spool files -> MySQL ---------- */
enum UpResult { UP_OK, UP_RETRY, UP_REJECT };

/* A failure that repeating cannot fix (our SIGNALs, constraint or data
 * errors) must not block the segments behind it; anything else is treated as
 * a connection problem and retried later. */
static UpResult classify(unsigned err)
{
    switch (err) {
    case 1644:                      /* SIGNAL from our procedures/triggers */
    case 1062: case 1452:           /* duplicate key, foreign key */
    case 1406: case 1264: case 1366:/* data too long / out of range / bad value */
        return UP_REJECT;
    default:
        return UP_RETRY;
    }
}

/* Send one segment inside ONE transaction. begin_segment() records the
 * segment's name in ingest_log in that same transaction, so even if we crash
 * after COMMIT but before deleting the file, sending it again is recognised
 * and skipped: every segment reaches the database exactly once. */
static UpResult upload_segment(MYSQL *conn, const fs::path &path, long long *rows)
{
    Segment seg;
    if (!parse_segment(path, seg)) {
        fprintf(stderr, "spool: %s is not a valid segment\n", path.filename().c_str());
        return UP_REJECT;
    }

    int sid = 0, already = 0;
    std::string q = "CALL begin_segment('" + seg.uuid + "', '" + esc(conn, seg.machine) + "', "
                  + std::to_string(seg.start) + ", '" + esc(conn, path.stem().string()) + "')";
    if (!call_proc(conn, q, &sid, &already) || sid <= 0) {
        unsigned e = mysql_errno(conn);
        fprintf(stderr, "begin_segment failed: %s\n", mysql_error(conn));
        mysql_rollback(conn);
        return classify(e);
    }
    if (already) {                        /* delivered before: nothing to do */
        mysql_commit(conn);
        return UP_OK;
    }

    MYSQL_STMT *ev = mysql_stmt_init(conn);
    MYSQL_STMT *sn = mysql_stmt_init(conn);
    const char *ev_sql =
        "INSERT INTO process_events "
        "(session_id, event_type, pid, ppid, process_name, event_time) "
        "VALUES (?, ?, ?, ?, ?, FROM_UNIXTIME(? / 1000000))";
    const char *sn_sql =
        "INSERT INTO process_snapshots "
        "(session_id, process_name, pid, cpu_usage, memory_usage_mb, is_idle, log_time) "
        "VALUES (?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?))";
    auto fail = [&](unsigned err, const char *msg) {
        fprintf(stderr, "upload failed: %s\n", msg);
        mysql_stmt_close(ev);
        mysql_stmt_close(sn);
        mysql_rollback(conn);
        return classify(err);
    };
    if (mysql_stmt_prepare(ev, ev_sql, strlen(ev_sql)))
        return fail(mysql_stmt_errno(ev), mysql_stmt_error(ev));
    if (mysql_stmt_prepare(sn, sn_sql, strlen(sn_sql)))
        return fail(mysql_stmt_errno(sn), mysql_stmt_error(sn));

    int e_sid = sid, e_pid = 0, e_ppid = 0;
    long long e_t = 0;
    char e_type[16] = "", e_name[256] = "";
    unsigned long e_type_len = 0, e_name_len = 0;
    MYSQL_BIND eb[6];
    memset(eb, 0, sizeof eb);
    eb[0].buffer_type = MYSQL_TYPE_LONG;     eb[0].buffer = &e_sid;
    eb[1].buffer_type = MYSQL_TYPE_STRING;   eb[1].buffer = e_type; eb[1].length = &e_type_len;
    eb[2].buffer_type = MYSQL_TYPE_LONG;     eb[2].buffer = &e_pid;
    eb[3].buffer_type = MYSQL_TYPE_LONG;     eb[3].buffer = &e_ppid;
    eb[4].buffer_type = MYSQL_TYPE_STRING;   eb[4].buffer = e_name; eb[4].length = &e_name_len;
    eb[5].buffer_type = MYSQL_TYPE_LONGLONG; eb[5].buffer = &e_t;
    mysql_stmt_bind_param(ev, eb);

    int s_sid = sid, s_pid = 0, s_idle = 0;
    long long s_t = 0;
    double s_cpu = 0, s_rss = 0;
    char s_name[256] = "";
    unsigned long s_name_len = 0;
    MYSQL_BIND sb[7];
    memset(sb, 0, sizeof sb);
    sb[0].buffer_type = MYSQL_TYPE_LONG;     sb[0].buffer = &s_sid;
    sb[1].buffer_type = MYSQL_TYPE_STRING;   sb[1].buffer = s_name; sb[1].length = &s_name_len;
    sb[2].buffer_type = MYSQL_TYPE_LONG;     sb[2].buffer = &s_pid;
    sb[3].buffer_type = MYSQL_TYPE_DOUBLE;   sb[3].buffer = &s_cpu;
    sb[4].buffer_type = MYSQL_TYPE_DOUBLE;   sb[4].buffer = &s_rss;
    sb[5].buffer_type = MYSQL_TYPE_LONG;     sb[5].buffer = &s_idle;
    sb[6].buffer_type = MYSQL_TYPE_LONGLONG; sb[6].buffer = &s_t;
    mysql_stmt_bind_param(sn, sb);

    for (const Record &r : seg.recs) {
        if (r.kind == 'E') {
            snprintf(e_type, sizeof e_type, "%s", r.type.c_str());
            snprintf(e_name, sizeof e_name, "%s", r.name.c_str());
            e_type_len = strlen(e_type);
            e_name_len = strlen(e_name);
            e_pid = r.pid;
            e_ppid = r.ppid;
            e_t = r.t;
            if (mysql_stmt_execute(ev))
                return fail(mysql_stmt_errno(ev), mysql_stmt_error(ev));
        } else {
            snprintf(s_name, sizeof s_name, "%s", r.name.c_str());
            s_name_len = strlen(s_name);
            s_pid = r.pid;
            s_cpu = r.cpu;
            s_rss = r.rss_mb;
            s_idle = r.idle;
            s_t = r.t;
            if (mysql_stmt_execute(sn))
                return fail(mysql_stmt_errno(sn), mysql_stmt_error(sn));
        }
    }
    mysql_stmt_close(ev);
    mysql_stmt_close(sn);

    if (seg.has_end) {                    /* last segment of the run: close the session */
        std::string e = "CALL end_session_at(" + std::to_string(sid) + ", "
                      + std::to_string(seg.end) + ")";
        if (!call_proc(conn, e)) {
            unsigned err = mysql_errno(conn);
            fprintf(stderr, "end_session_at failed: %s\n", mysql_error(conn));
            mysql_rollback(conn);
            return classify(err);
        }
    }
    if (mysql_commit(conn)) {
        fprintf(stderr, "commit failed: %s\n", mysql_error(conn));
        return UP_RETRY;
    }
    *rows += (long long)seg.recs.size();
    return UP_OK;
}

static void uploader_thread()
{
    mysql_thread_init();
    MYSQL *conn = nullptr;
    int backoff = 1, sent = 0;
    long long rows = 0;
    bool was_down = false;

    for (;;) {
        bool stopping = up_stopping();
        std::vector<fs::path> segs = list_segments(g_spool, ".ready");
        if (segs.empty()) {
            if (stopping) break;
            up_wait(2, true);
            continue;
        }
        if (!conn) {
            conn = db_connect();
            if (!conn) {
                was_down = true;
                if (stopping) break;      /* the spool keeps it for the next run */
                up_wait(backoff, false);
                backoff = std::min(backoff * 2, 30);   /* exponential backoff */
                continue;
            }
            if (was_down)
                fprintf(stderr, "database reachable again, %zu segment(s) to send\n", segs.size());
            was_down = false;
            backoff = 1;
        }

        UpResult r = upload_segment(conn, segs.front(), &rows);
        std::error_code ec;
        if (r == UP_OK) {
            fs::remove(segs.front(), ec);
            sent++;
            continue;
        }
        if (r == UP_REJECT) {             /* keep the file for inspection, move on */
            fs::path bad = segs.front();
            bad.replace_extension(".rejected");
            fs::rename(segs.front(), bad, ec);
            fprintf(stderr, "spool: %s rejected by the server, kept as %s\n",
                    segs.front().filename().c_str(), bad.filename().c_str());
            continue;
        }
        mysql_close(conn);                /* UP_RETRY: connection trouble */
        conn = nullptr;
        was_down = true;
        if (stopping) break;
        up_wait(backoff, false);
        backoff = std::min(backoff * 2, 30);
    }
    if (conn) mysql_close(conn);
    mysql_thread_end();
    printf("uploader: %lld rows in %d segment(s) delivered, %zu segment(s) left in spool\n",
           rows, sent, list_segments(g_spool, ".ready").size());
}

/* ---------- producer 2 (main thread): kernel events -> queue ---------- */
static long long read_events()
{
    int fd = open("/dev/activity_logger", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/activity_logger (module loaded? run as root)");
        return -1;
    }

    std::string pending;
    char buf[4096];
    long long count = 0;
    bool alive = true;

    while (!g_stop && alive) {
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

            char type[16] = "", name[256] = "";
            int pid = 0, ppid = 0;
            long long us = 0;
            if (sscanf(line.c_str(), "%15s %d %d %lld %255[^\n]",
                       type, &pid, &ppid, &us, name) != 5)
                continue;
            if (pid == getpid())
                continue;

            Record r;
            r.kind = 'E';
            r.type = type;
            r.pid  = pid;
            r.ppid = ppid;
            r.t    = us;
            r.name = name;
            if (!push_record(std::move(r))) {
                alive = false;
                break;
            }
            count++;
        }
    }
    close(fd);
    return count;
}

static std::string new_uuid()
{
    char buf[64] = "";
    FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");
    if (f) {
        if (!fgets(buf, sizeof buf, f))
            buf[0] = '\0';
        fclose(f);
    }
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (!load_agent_config(env_or("ACT_CONFIG", "/etc/activity-logger/agent.conf"),
                           g_cfg))
        return 1;

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

    char hostname[100];
    gethostname(hostname, sizeof hostname);
    hostname[sizeof hostname - 1] = '\0';
    g_machine = hostname;
    g_run_uuid = new_uuid();
    g_run_start = (long long)time(nullptr);
    if (!valid_uuid(g_run_uuid)) {
        fprintf(stderr, "cannot generate a run id\n");
        return 1;
    }

    /* Local spool: private to root, and checked for leftovers of a crashed run */
    g_spool = g_cfg.spool_dir;
    std::error_code ec;
    fs::create_directories(g_spool, ec);
    fs::permissions(g_spool, fs::perms::owner_all, ec);
    if (!fs::is_directory(g_spool)) {
        fprintf(stderr, "cannot create spool directory %s\n", g_spool.c_str());
        return 1;
    }
    int recovered = recover_spool(g_spool);
    if (recovered)
        printf("spool: closed %d unfinished run(s) from an earlier crash\n", recovered);
    size_t waiting = list_segments(g_spool, ".ready").size();
    if (waiting)
        printf("spool: %zu segment(s) waiting to be sent\n", waiting);

    std::thread spooler(spooler_thread);
    std::thread sampler(sampler_thread);
    std::thread uploader(uploader_thread);

    idle_start();

    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);     /* main handles signals */

    printf("run %s started (Ctrl+C to stop)\n", g_run_uuid.c_str());
    long long events = read_events();

    idle_stop();

    /* Shut down in order: producers, then the spooler (which writes the end
     * marker), then the uploader (which makes one last delivery attempt). */
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_done = true;
    }
    g_cv_stop.notify_all();
    g_cv_not_full.notify_all();
    g_cv_not_empty.notify_all();
    sampler.join();
    spooler.join();
    {
        std::lock_guard<std::mutex> lk(g_up_mtx);
        g_up_stop = true;
    }
    g_up_cv.notify_all();
    uploader.join();

    mysql_library_end();
    printf("run %s closed, %lld events recorded\n", g_run_uuid.c_str(), events);
    return events < 0 ? 1 : 0;
}
