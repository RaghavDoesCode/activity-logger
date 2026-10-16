#pragma once
#include <linux/input.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>

/* Time of the last keyboard/mouse event, in CLOCK_MONOTONIC milliseconds */
static std::atomic<long long> g_last_input_ms{0};
static std::atomic<bool> g_idle_stop{false};
static std::thread g_idle_thread;

static inline long long mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline void idle_loop()
{
    /* Open every input device the kernel exposes (needs root) */
    std::vector<int> fds;
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (strncmp(e->d_name, "event", 5) != 0)
                continue;
            char path[300];
            snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0)
                fds.push_back(fd);
        }
        closedir(d);
    }
    if (fds.empty())
        fprintf(stderr, "idle: no input devices readable (run as root)\n");
    fprintf(stderr, "idle: watching %zu input devices\n", fds.size());
    std::vector<struct pollfd> pfds(fds.size());
    for (size_t i = 0; i < fds.size(); i++) {
        pfds[i].fd = fds[i];
        pfds[i].events = POLLIN;
    }

    while (!g_idle_stop) {
        /* poll() sleeps until ANY device has data, or 500 ms pass */
        int r = poll(pfds.data(), pfds.size(), 500);
        if (r <= 0)
            continue;

        for (size_t i = 0; i < pfds.size(); i++) {
            if (!(pfds[i].revents & POLLIN))
                continue;
            struct input_event ev;
            while (read(pfds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY || ev.type == EV_REL || ev.type == EV_ABS)
                    g_last_input_ms = mono_ms();
            }
        }
    }
    for (int fd : fds)
        close(fd);
}

static inline void idle_start()
{
    g_last_input_ms = mono_ms();          /* assume the user is active at start */
    g_idle_thread = std::thread(idle_loop);
}

static inline void idle_stop()
{
    g_idle_stop = true;
    if (g_idle_thread.joinable())
        g_idle_thread.join();
}

static inline double idle_seconds()
{
    return (mono_ms() - g_last_input_ms.load()) / 1000.0;
}
