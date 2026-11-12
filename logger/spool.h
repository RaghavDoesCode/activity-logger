#pragma once
/* The spool: a directory of small text files ("segments") that holds
 * everything the logger has observed but not yet delivered to MySQL.
 *
 *   seg-<id>.open   being written right now
 *   seg-<id>.ready  complete; waiting for the uploader
 *
 * Every segment starts with a header line naming the run it belongs to, so it
 * can be uploaded on its own, even after a crash or a reboot. */
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/* One thing the logger observed */
struct Record {
    char kind = 'P';            /* 'E' = process event, 'P' = process snapshot */
    std::string type;           /* events only: "EXEC" or "EXIT" */
    int pid = 0, ppid = 0;
    long long t = 0;            /* events: unix microseconds; snapshots: unix seconds */
    double cpu = 0, rss_mb = 0;
    int idle = 0;
    std::string name;
};

/* The content of one segment file */
struct Segment {
    std::string uuid, machine;
    long long start = 0;
    bool has_end = false;
    long long end = 0;
    std::vector<Record> recs;
};

/* Fields are separated by tabs and records by newlines, so neither may
 * appear inside a process name. */
static inline std::string spool_clean(const std::string &s)
{
    std::string o = s;
    for (char &c : o)
        if (c == '\t' || c == '\n' || c == '\r')
            c = '?';
    return o;
}

static inline std::string record_line(const Record &r)
{
    char buf[256];
    if (r.kind == 'E')
        snprintf(buf, sizeof buf, "E\t%s\t%d\t%d\t%lld\t",
                 r.type.c_str(), r.pid, r.ppid, r.t);
    else
        snprintf(buf, sizeof buf, "P\t%lld\t%d\t%.3f\t%.3f\t%d\t",
                 r.t, r.pid, r.cpu, r.rss_mb, r.idle);
    return std::string(buf) + spool_clean(r.name) + "\n";
}

/* Segment ids are microsecond timestamps, forced to increase, so sorting the
 * file names gives upload order. */
static inline long long next_segment_id()
{
    static std::atomic<long long> last{0};
    long long now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long prev = last.load(), id;
    do {
        id = now > prev ? now : prev + 1;
    } while (!last.compare_exchange_weak(prev, id));
    return id;
}

static inline std::string segment_name(long long id)
{
    char b[48];
    snprintf(b, sizeof b, "seg-%020lld", id);
    return b;
}

static inline std::vector<fs::path> list_segments(const fs::path &dir, const char *ext)
{
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ext)
            v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}

static inline std::vector<std::string> split_tabs(const std::string &s)
{
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        size_t q = s.find('\t', p);
        if (q == std::string::npos) {
            out.push_back(s.substr(p));
            break;
        }
        out.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

static inline bool valid_uuid(const std::string &u)
{
    if (u.size() != 36)
        return false;
    for (char c : u)
        if (!isxdigit((unsigned char)c) && c != '-')
            return false;
    return true;
}

/* Lenient on purpose: after a crash the last line may be cut short, and such
 * a line is simply skipped. Returns false if there is no usable header. */
static inline bool parse_segment(const fs::path &p, Segment &seg)
{
    std::ifstream in(p);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        std::vector<std::string> f = split_tabs(line);
        if (f[0] == "H" && f.size() == 4 && valid_uuid(f[1])) {
            seg.uuid = f[1];
            seg.start = atoll(f[2].c_str());
            seg.machine = f[3];
            header = true;
        } else if (f[0] == "E" && f.size() == 6 && (f[1] == "EXEC" || f[1] == "EXIT")) {
            Record r;
            r.kind = 'E';
            r.type = f[1];
            r.pid = atoi(f[2].c_str());
            r.ppid = atoi(f[3].c_str());
            r.t = atoll(f[4].c_str());
            r.name = f[5].substr(0, 255);
            seg.recs.push_back(std::move(r));
        } else if (f[0] == "P" && f.size() == 7) {
            Record r;
            r.kind = 'P';
            r.t = atoll(f[1].c_str());
            r.pid = atoi(f[2].c_str());
            r.cpu = atof(f[3].c_str());
            r.rss_mb = atof(f[4].c_str());
            r.idle = atoi(f[5].c_str());
            r.name = f[6].substr(0, 255);
            seg.recs.push_back(std::move(r));
        } else if (f[0] == "X" && f.size() == 2) {
            seg.has_end = true;
            seg.end = atoll(f[1].c_str());
        }
    }
    return header;
}

/* Appends records to the current segment and hands finished segments over to
 * the uploader by renaming them from .open to .ready (a rename is atomic, so
 * the uploader never sees a half-written file). */
class SpoolWriter {
public:
    SpoolWriter(fs::path dir, std::string uuid, long long start,
                std::string machine, double max_mb)
        : dir_(std::move(dir)), uuid_(std::move(uuid)), start_(start),
          machine_(spool_clean(std::move(machine))),
          cap_bytes_((unsigned long long)(max_mb * 1024 * 1024)) {}

    ~SpoolWriter() { rotate(); }

    void append(const Record &r)
    {
        if (!f_)
            open_segment();
        if (!f_) {
            dropped_++;
            return;
        }
        std::string line = record_line(r);
        fwrite(line.data(), 1, line.size(), f_);
        nrec_++;
    }

    /* Push buffered data to the operating system. After this, even a crash
     * of the logger (kill -9) cannot lose it; only a power cut could. */
    void flush()
    {
        if (f_)
            fflush(f_);
    }

    bool should_rotate() const
    {
        return f_ && (nrec_ >= 2000 || time(nullptr) - opened_ >= 5);
    }

    /* Make the segment durable and visible to the uploader */
    void rotate()
    {
        if (!f_)
            return;
        fflush(f_);
        fsync(fileno(f_));
        fclose(f_);
        f_ = nullptr;
        fs::path ready = open_path_;
        ready.replace_extension(".ready");
        std::error_code ec;
        fs::rename(open_path_, ready, ec);
        if (ec)
            fprintf(stderr, "spool: rename failed: %s\n", ec.message().c_str());
        enforce_cap();
    }

    /* The run is over: record when, so the server can close the session */
    void finish(long long end_unix)
    {
        if (!f_)
            open_segment();
        if (f_)
            fprintf(f_, "X\t%lld\n", end_unix);
        rotate();
    }

    unsigned long long dropped() const { return dropped_; }
    unsigned long long dropped_segments() const { return dropped_segments_; }

private:
    void open_segment()
    {
        open_path_ = dir_ / (segment_name(next_segment_id()) + ".open");
        f_ = fopen(open_path_.c_str(), "w");
        if (!f_) {
            perror("spool: cannot create segment");
            return;
        }
        fprintf(f_, "H\t%s\t%lld\t%s\n", uuid_.c_str(), start_, machine_.c_str());
        nrec_ = 0;
        opened_ = time(nullptr);
    }

    /* The disk is not unlimited: if the server stays away for very long,
     * throw away the OLDEST finished segments first. */
    void enforce_cap()
    {
        std::error_code ec;
        std::vector<fs::path> segs = list_segments(dir_, ".ready");
        unsigned long long total = 0;
        for (const auto &s : segs)
            total += fs::file_size(s, ec);
        while (total > cap_bytes_ && segs.size() > 1) {
            total -= fs::file_size(segs.front(), ec);
            fs::remove(segs.front(), ec);
            segs.erase(segs.begin());
            dropped_segments_++;
            fprintf(stderr, "spool: over limit, dropped oldest segment\n");
        }
    }

    fs::path dir_, open_path_;
    std::string uuid_;
    long long start_;
    std::string machine_;
    unsigned long long cap_bytes_;
    FILE *f_ = nullptr;
    size_t nrec_ = 0;
    time_t opened_ = 0;
    unsigned long long dropped_ = 0, dropped_segments_ = 0;
};

/* Run at startup, before anything new is written. A run that crashed never
 * wrote its end marker, so its session would stay open forever. We close it
 * here, using the modification time of its last segment as the end time.
 * Returns how many crashed runs were closed. */
static inline int recover_spool(const fs::path &dir)
{
    std::error_code ec;

    /* a crash leaves the last segment as .open: it is complete enough to send */
    for (const auto &p : list_segments(dir, ".open")) {
        if (fs::file_size(p, ec) == 0) {          /* nothing was written: just remove it */
            fs::remove(p, ec);
            continue;
        }
        fs::path r = p;
        r.replace_extension(".ready");
        fs::rename(p, r, ec);
    }

    struct Run { std::string uuid, machine; long long start = 0; bool ended = false; time_t last = 0; };
    std::map<std::string, Run> runs;
    for (const auto &p : list_segments(dir, ".ready")) {
        std::ifstream in(p);
        std::string line, first, last;
        while (std::getline(in, line)) {
            if (first.empty())
                first = line;
            if (!line.empty())
                last = line;
        }
        std::vector<std::string> h = split_tabs(first);
        if (h.size() != 4 || h[0] != "H" || !valid_uuid(h[1]))
            continue;
        Run &r = runs[h[1]];
        if (r.uuid.empty()) {
            r.uuid = h[1];
            r.start = atoll(h[2].c_str());
            r.machine = h[3];
        }
        if (last.rfind("X\t", 0) == 0)
            r.ended = true;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && st.st_mtime > r.last)
            r.last = st.st_mtime;
    }

    int closed = 0;
    for (const auto &kv : runs) {
        const Run &r = kv.second;
        if (r.ended)
            continue;
        fs::path open = dir / (segment_name(next_segment_id()) + ".open");
        FILE *f = fopen(open.c_str(), "w");
        if (!f)
            continue;
        fprintf(f, "H\t%s\t%lld\t%s\n", r.uuid.c_str(), r.start, r.machine.c_str());
        fprintf(f, "X\t%lld\n", (long long)(r.last > r.start ? r.last : r.start));
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        fs::path ready = open;
        ready.replace_extension(".ready");
        fs::rename(open, ready, ec);
        closed++;
    }
    return closed;
}
