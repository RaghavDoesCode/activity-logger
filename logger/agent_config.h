#pragma once
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

/* Per-device settings, read from /etc/activity-logger/agent.conf */
struct AgentConfig {
    std::string host, db, user, pass, ca, spool_dir;
    unsigned int port = 3306;
    double spool_max_mb = 200;          /* cap on the local backlog */
};

static inline std::string cfg_trim(const std::string &s)
{
    const char *ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

/* Reads KEY=VALUE lines ('#' starts a comment). The file holds a password,
 * so, like ssh with private keys, we refuse to run if it is not owned by the
 * user running us or if group/others can read it. */
static inline bool load_agent_config(const std::string &path, AgentConfig &cfg)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        perror(path.c_str());
        return false;
    }
    if (st.st_uid != geteuid() || (st.st_mode & 077)) {
        fprintf(stderr, "%s must be owned by the user running the logger "
                        "and have mode 0600\n", path.c_str());
        return false;
    }

    std::ifstream in(path);
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        line = cfg_trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        kv[cfg_trim(line.substr(0, eq))] = cfg_trim(line.substr(eq + 1));
    }

    auto get = [&](const char *key, const char *def) {
        auto it = kv.find(key);
        return it == kv.end() ? std::string(def) : it->second;
    };
    cfg.host = get("DB_HOST", "");
    cfg.db   = get("DB_NAME", "activity_monitor");
    cfg.user = get("DB_USER", "");
    cfg.pass = get("DB_PASS", "");
    cfg.ca   = get("DB_CA",   "/etc/activity-logger/ca.pem");
    cfg.port = (unsigned int)strtoul(get("DB_PORT", "3306").c_str(), nullptr, 10);
    cfg.spool_dir = get("SPOOL_DIR", "/var/lib/activity-logger/spool");
    cfg.spool_max_mb = strtod(get("SPOOL_MAX_MB", "200").c_str(), nullptr);

    if (cfg.host.empty() || cfg.user.empty() || cfg.pass.empty()) {
        fprintf(stderr, "config: DB_HOST, DB_USER and DB_PASS are required\n");
        return false;
    }
    /* optional: idle threshold in seconds (the sampler reads this variable) */
    auto it = kv.find("IDLE_SECONDS");
    if (it != kv.end())
        setenv("ACT_IDLE_SECONDS", it->second.c_str(), 0);
    return true;
}
