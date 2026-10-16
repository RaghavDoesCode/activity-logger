#pragma once
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

struct Sample {
    int pid = 0;
    std::string name;
    unsigned long ticks = 0;   /* user + kernel CPU time, in clock ticks */
    double rss_mb = 0;         /* resident memory in MB */
};

/* Read /proc/<pid>/stat and /proc/<pid>/statm.
 * Returns false if the process vanished or is a kernel thread. */
static inline bool read_proc(int pid, Sample &s)
{
    char path[64], buf[1024];

    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* The name is in parentheses and may itself contain spaces or
     * parentheses, so find the FIRST '(' and the LAST ')'. */
    char *lparen = strchr(buf, '(');
    char *rparen = strrchr(buf, ')');
    if (!lparen || !rparen) return false;
    s.name.assign(lparen + 1, rparen - lparen - 1);

    /* After the name: state, then 5 ints, 5 unsigned skipped,
     * then utime (field 14) and stime (field 15) */
    char state;
    unsigned long utime, stime;
    if (sscanf(rparen + 2,
               "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &state, &utime, &stime) != 3)
        return false;
    s.ticks = utime + stime;
    s.pid = pid;

    /* statm: size resident shared ...   (all counted in pages) */
    snprintf(path, sizeof path, "/proc/%d/statm", pid);
    f = fopen(path, "r");
    if (!f) return false;
    unsigned long rss_pages = 0;
    int ok = fscanf(f, "%*u %lu", &rss_pages);
    fclose(f);
    if (ok != 1 || rss_pages == 0)      /* kernel threads use no user memory */
        return false;

    s.rss_mb = rss_pages * (double)sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0);
    return true;
}

/* Every numeric directory in /proc is one running process */
static inline std::map<int, Sample> scan_all()
{
    std::map<int, Sample> all;
    DIR *d = opendir("/proc");
    if (!d) return all;

    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        Sample s;
        if (read_proc(atoi(e->d_name), s))
            all[s.pid] = s;
    }
    closedir(d);
    return all;
}

static inline double now_sec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

