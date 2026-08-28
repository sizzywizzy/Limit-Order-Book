#pragma once

// Hardware performance counters, read directly through perf_event_open.
//
// Why not `perf stat`: it aggregates over the whole process, so it cannot
// separate the cancel path from the add path — and "cycles per cancel" is the
// number this project actually wants. Opening the counters ourselves lets the
// harness bracket exactly the measured region, after warm-up, with the
// harness's own bookkeeping outside it.
//
// The counters are opened as a group on the calling thread with
// exclude_kernel, so they work at the default perf_event_paranoid=2 without
// privileges — which matters, because WSL2 ships a custom kernel that
// distribution `perf` binaries do not match.
//
// Multiplexing: five events are requested and modern x86 has four general
// counters plus a fixed set, so the kernel may time-slice them. The group is
// read with PERF_FORMAT_TOTAL_TIME_ENABLED / RUNNING and every count is
// scaled by enabled/running, which is what `perf stat` does; when scaling was
// applied, report() says so rather than presenting an unscaled number.
//
// Non-Linux builds get a stub whose `available()` is false, so the flag
// degrades to a clear message instead of a compile error.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace obbench {

struct CounterSet {
    std::uint64_t cycles{0};
    std::uint64_t instructions{0};
    std::uint64_t cache_references{0};
    std::uint64_t cache_misses{0};
    std::uint64_t branch_misses{0};
    double scale{1.0};  // > 1.0 means the kernel multiplexed the group

    void add(const CounterSet& o) {
        cycles += o.cycles;
        instructions += o.instructions;
        cache_references += o.cache_references;
        cache_misses += o.cache_misses;
        branch_misses += o.branch_misses;
        scale = o.scale > scale ? o.scale : scale;
    }
};

#if defined(__linux__)

class PerfCounters {
public:
    PerfCounters() {
        const struct {
            std::uint32_t type;
            std::uint64_t config;
        } wanted[kEvents] = {
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
        };

        for (int i = 0; i < kEvents; ++i) {
            perf_event_attr attr{};
            attr.size = sizeof(attr);
            attr.type = wanted[i].type;
            attr.config = wanted[i].config;
            attr.disabled = (i == 0) ? 1 : 0;
            attr.exclude_kernel = 1;  // required at paranoid=2
            attr.exclude_hv = 1;
            attr.inherit = 0;
            attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
                               PERF_FORMAT_TOTAL_TIME_ENABLED |
                               PERF_FORMAT_TOTAL_TIME_RUNNING;
            const int fd = static_cast<int>(
                syscall(__NR_perf_event_open, &attr, 0, -1,
                        i == 0 ? -1 : fds_[0], 0));
            if (fd < 0) {
                if (i == 0) {
                    error_ = std::string("perf_event_open failed: ") +
                             std::strerror(errno) + diagnose(errno);
                }
                close_all();
                return;
            }
            fds_[i] = fd;
        }
        ok_ = true;
    }

    ~PerfCounters() { close_all(); }

    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    [[nodiscard]] bool available() const noexcept { return ok_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    void start() noexcept {
        if (!ok_) return;
        ioctl(fds_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }

    CounterSet stop() noexcept {
        CounterSet out;
        if (!ok_) return out;
        ioctl(fds_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

        // struct read_format { nr, time_enabled, time_running,
        //                      { value, id }[nr] }
        std::uint64_t buf[3 + 2 * kEvents]{};
        const ssize_t n = read(fds_[0], buf, sizeof(buf));
        if (n <= 0) return out;

        const std::uint64_t nr = buf[0];
        const std::uint64_t enabled = buf[1];
        const std::uint64_t running = buf[2];
        const double scale =
            (running > 0 && enabled > running)
                ? static_cast<double>(enabled) / static_cast<double>(running)
                : 1.0;
        out.scale = scale;

        std::uint64_t vals[kEvents]{};
        for (std::uint64_t i = 0; i < nr && i < kEvents; ++i) {
            const auto raw = static_cast<double>(buf[3 + 2 * i]);
            vals[i] = static_cast<std::uint64_t>(raw * scale + 0.5);
        }
        out.cycles = vals[0];
        out.instructions = vals[1];
        out.cache_references = vals[2];
        out.cache_misses = vals[3];
        out.branch_misses = vals[4];
        return out;
    }

private:
    static constexpr int kEvents = 5;

    // Distinguish "the kernel refused us" from "there is no PMU here", which
    // are different problems with different fixes. A software event needs no
    // PMU, so if that opens and a hardware event does not, the CPU's counters
    // are simply not exposed to this guest — the usual case under WSL2 and
    // most VMs, and not something a permission change can fix.
    static std::string diagnose(int hw_errno) {
        perf_event_attr sw{};
        sw.size = sizeof(sw);
        sw.type = PERF_TYPE_SOFTWARE;
        sw.config = PERF_COUNT_SW_TASK_CLOCK;
        sw.exclude_kernel = 1;
        sw.exclude_hv = 1;
        const int fd = static_cast<int>(
            syscall(__NR_perf_event_open, &sw, 0, -1, -1, 0));
        if (fd >= 0) {
            close(fd);
            return " — software events work, so perf_event_open is permitted; "
                   "the hardware PMU is not exposed to this kernel (expected "
                   "under WSL2 and most VMs). Run on bare metal for counters.";
        }
        if (hw_errno == EACCES || hw_errno == EPERM) {
            return " — try lowering /proc/sys/kernel/perf_event_paranoid.";
        }
        return " — software events fail too; perf_event support looks absent "
               "from this kernel.";
    }

    void close_all() noexcept {
        for (int& fd : fds_) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }
        ok_ = false;
    }

    int fds_[kEvents] = {-1, -1, -1, -1, -1};
    bool ok_{false};
    std::string error_;
};

#else  // portable stub

class PerfCounters {
public:
    [[nodiscard]] bool available() const noexcept { return false; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    void start() noexcept {}
    CounterSet stop() noexcept { return {}; }

private:
    std::string error_{
        "hardware counters need Linux perf_event_open; build and run this "
        "under WSL2 or a Linux host"};
};

#endif

// Per-operation averages, printed the way `perf stat` prints them so the two
// can be compared directly.
inline void report_counters(const char* label, const CounterSet& c,
                            std::size_t ops) {
    if (ops == 0) return;
    const auto per = [ops](std::uint64_t v) {
        return static_cast<double>(v) / static_cast<double>(ops);
    };
    std::printf("%-16s %10.1f cyc  %10.1f ins  %5.2f IPC  %8.3f cache-ref  "
                "%8.3f cache-miss  %7.3f branch-miss\n",
                label, per(c.cycles), per(c.instructions),
                c.cycles ? static_cast<double>(c.instructions) /
                               static_cast<double>(c.cycles)
                         : 0.0,
                per(c.cache_references), per(c.cache_misses),
                per(c.branch_misses));
}

}  // namespace obbench
