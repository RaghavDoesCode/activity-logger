#include <mysql/mysql.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static std::string env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v ? v : def;
}

int main()
{
    std::string host = env_or("ACT_DB_HOST", "localhost");
    std::string user = env_or("ACT_DB_USER", "monitor_user");
    std::string pass = env_or("ACT_DB_PASS", "");
    std::string db   = env_or("ACT_DB_NAME", "activity_monitor");
    std::string emp  = env_or("ACT_EMPLOYEE", "EMP001");

    /* Signals: set a flag instead of dying, so we can close the session.
     * No SA_RESTART, so a blocked read() returns with EINTR. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    /* Connect to MySQL */
    MYSQL *conn = mysql_init(nullptr);
    if (!mysql_real_connect(conn, host.c_str(), user.c_str(), pass.c_str(),
                            db.c_str(), 3306, nullptr, 0)) {
        fprintf(stderr, "connect failed: %s\n", mysql_error(conn));
        return 1;
    }
    mysql_autocommit(conn, 0);              /* we commit in batches */

    auto esc = [&](const std::string &s) {
        std::string out(2 * s.size() + 1, '\0');
        unsigned long n = mysql_real_escape_string(conn, &out[0],
                                                   s.c_str(), s.size());
        out.resize(n);
        return out;
    };

    /* Start a session row */
    char hostname[100];
    gethostname(hostname, sizeof hostname);
    hostname[sizeof hostname - 1] = '\0';
    std::string q = "INSERT INTO sessions (employee_id, machine_name) VALUES ('"
                  + esc(emp) + "', '" + esc(hostname) + "')";
    if (mysql_query(conn, q.c_str())) {
        fprintf(stderr, "session insert failed: %s\n", mysql_error(conn));
        return 1;
    }
    int session_id = (int)mysql_insert_id(conn);   /* read before COMMIT */
    mysql_commit(conn);

    /* Prepared statement: parsed once, executed for every event */
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    const char *sql =
        "INSERT INTO process_events "
        "(session_id, event_type, pid, ppid, process_name, event_time) "
        "VALUES (?, ?, ?, ?, ?, FROM_UNIXTIME(? / 1000000))";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        fprintf(stderr, "prepare failed: %s\n", mysql_stmt_error(stmt));
        return 1;
    }

    int p_sid = session_id, p_pid = 0, p_ppid = 0;
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

    /* Open the kernel device: a plain file descriptor + read() syscalls */
    int fd = open("/dev/activity_logger", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/activity_logger (module loaded? run with sudo)");
        return 1;
    }

    printf("session %d started, reading events (Ctrl+C to stop)\n", session_id);

    std::string pending;                    /* holds a partial line between reads */
    char buf[4096];
    long long stored = 0;

    while (!g_stop) {
        ssize_t n = read(fd, buf, sizeof buf);   /* blocks until data arrives */
        if (n < 0) {
            if (errno == EINTR) continue;        /* signal: re-check g_stop */
            perror("read");
            break;
        }
        if (n == 0) break;
        pending.append(buf, n);

        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);

            /* "EXEC 7890 4549 1792134012834574 name with spaces" */
            if (sscanf(line.c_str(), "%15s %d %d %lld %255[^\n]",
                       p_type, &p_pid, &p_ppid, &p_us, p_name) != 5)
                continue;                        /* malformed line */
            if (p_pid == getpid())
                continue;                        /* ignore our own events */

            type_len = strlen(p_type);
            name_len = strlen(p_name);
            if (mysql_stmt_execute(stmt)) {
                fprintf(stderr, "insert failed: %s\n", mysql_stmt_error(stmt));
                continue;
            }
            stored++;
        }
        mysql_commit(conn);                      /* one transaction per batch */
    }

    /* Close the session */
    close(fd);
    mysql_stmt_close(stmt);
    std::string e = "UPDATE sessions SET end_time = NOW() WHERE session_id = "
                  + std::to_string(session_id);
    mysql_query(conn, e.c_str());
    mysql_commit(conn);
    mysql_close(conn);
    printf("session %d closed, %lld events stored\n", session_id, stored);
    return 0;
}
