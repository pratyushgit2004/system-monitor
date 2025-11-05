// main.cpp
// Simple System Monitor Tool (Linux) - reads /proc to show CPU, memory, uptime, and process list.
// Compile: g++ main.cpp -o system_monitor -std=c++17

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;
using ull = unsigned long long;
using clock_ms = chrono::steady_clock;

// ---------- Helpers to read files ----------
string readFirstLine(const string &path) {
    ifstream f(path);
    if (!f.is_open()) return "";
    string s;
    getline(f, s);
    return s;
}

vector<string> readAllLines(const string &path) {
    vector<string> lines;
    ifstream f(path);
    if (!f.is_open()) return lines;
    string s;
    while (getline(f, s)) lines.push_back(s);
    return lines;
}

// ---------- CPU totals from /proc/stat ----------
struct CpuSample {
    ull user = 0;
    ull nice = 0;
    ull system = 0;
    ull idle = 0;
    ull iowait = 0;
    ull irq = 0;
    ull softirq = 0;
    ull steal = 0;
    ull guest = 0;
    ull guest_nice = 0;

    ull total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
    }
    ull idleAll() const {
        return idle + iowait;
    }
};

bool parseProcStatCpu(const string &line, CpuSample &out) {
    // Expected line starting with "cpu " followed by numbers
    // Example: cpu  2255 34 2290 22625563 6290 127 456 0 0 0
    if (line.rfind("cpu ", 0) != 0) return false;
    stringstream ss(line);
    string cpuLabel;
    ss >> cpuLabel;
    ss >> out.user >> out.nice >> out.system >> out.idle >> out.iowait
       >> out.irq >> out.softirq >> out.steal >> out.guest >> out.guest_nice;
    return true;
}

CpuSample getCpuSample() {
    CpuSample s;
    string line = readFirstLine("/proc/stat");
    parseProcStatCpu(line, s);
    return s;
}

// ---------- Memory usage from /proc/meminfo ----------
struct MemInfo {
    ull memTotal = 0;
    ull memFree = 0;
    ull buffers = 0;
    ull cached = 0;
    ull available = 0;
};

MemInfo getMemInfo() {
    MemInfo m;
    auto lines = readAllLines("/proc/meminfo");
    for (const string &ln : lines) {
        string key;
        ull value = 0;
        string unit;
        stringstream ss(ln);
        ss >> key >> value >> unit; // key includes trailing ':'
        if (key == "MemTotal:") m.memTotal = value;
        else if (key == "MemFree:") m.memFree = value;
        else if (key == "Buffers:") m.buffers = value;
        else if (key == "Cached:") m.cached = value;
        else if (key == "MemAvailable:") m.available = value;
    }
    return m;
}

// ---------- System uptime ----------
double getUptimeSeconds() {
    string line = readFirstLine("/proc/uptime");
    if (line.empty()) return 0.0;
    double up = 0.0;
    stringstream ss(line);
    ss >> up;
    return up;
}

// ---------- Per-process info ----------
struct ProcessInfo {
    int pid = 0;
    string name;
    double cpuPercent = 0.0; // computed across samples
    ull totalTime = 0; // clock ticks (utime + stime)
    ull rss_kb = 0; // resident set size in KB
};

bool isDigits(const string &s) {
    return !s.empty() && all_of(s.begin(), s.end(), ::isdigit);
}

// Parse /proc/[pid]/stat carefully: second field is comm in parentheses and may contain spaces.
bool readProcPidStat(int pid, ull &utime, ull &stime, ull &rss_pages, string &comm) {
    string path = "/proc/" + to_string(pid) + "/stat";
    ifstream f(path);
    if (!f.is_open()) return false;

    // Read entire line
    string line;
    getline(f, line);
    f.close();
    if (line.empty()) return false;

    // Find parentheses around comm
    size_t l = line.find('(');
    size_t r = line.rfind(')');
    if (l == string::npos || r == string::npos || r <= l) return false;
    comm = line.substr(l + 1, r - l - 1);

    // After r+2 are remaining fields (state and many others)
    string after = line.substr(r + 2);
    stringstream ss(after);
    // fields after comm: state (1), ppid (2), pgrp (3), session (4), tty_nr (5), tpgid (6),
    // flags (7), minflt (8), cminflt (9), majflt (10), cmajflt (11),
    // utime (14), stime (15) ... but since we started after comm, we need to step to the correct offsets.
    // We'll parse tokens up to the relevant indices.
    vector<string> fields;
    string tok;
    while (ss >> tok) fields.push_back(tok);
    // utime is fields[13] (since fields[0] is state)
    if (fields.size() < 24) return false; // need at least up to rss
    try {
        utime = stoull(fields[13]);
        stime = stoull(fields[14]);
        // rss is at fields[21] (indexing from 0), number of pages
        rss_pages = stoull(fields[21]);
    } catch (...) {
        return false;
    }
    return true;
}

ull getPageSizeKb() {
    static ull ps = 0;
    if (ps == 0) {
        long pagesize = sysconf(_SC_PAGESIZE);
        ps = (pagesize > 0 ? (ull)pagesize / 1024ULL : 4); // in KB
    }
    return ps;
}

ProcessInfo readProcess(int pid) {
    ProcessInfo pi;
    pi.pid = pid;
    ull utime = 0, stime = 0, rss_pages = 0;
    string comm;
    if (readProcPidStat(pid, utime, stime, rss_pages, comm)) {
        pi.name = comm;
        pi.totalTime = utime + stime;
        ull pageKb = getPageSizeKb();
        pi.rss_kb = rss_pages * pageKb;
    } else {
        // fallback: try to read /proc/[pid]/comm and /proc/[pid]/status VmRSS
        string commPath = "/proc/" + to_string(pid) + "/comm";
        pi.name = readFirstLine(commPath);
        // status
        string statusPath = "/proc/" + to_string(pid) + "/status";
        auto lines = readAllLines(statusPath);
        for (const string &ln : lines) {
            if (ln.rfind("VmRSS:", 0) == 0) {
                stringstream s(ln);
                string key;
                ull val;
                string unit;
                s >> key >> val >> unit;
                pi.rss_kb = val;
            }
        }
        pi.totalTime = 0;
    }
    return pi;
}

// ---------- Read all PIDs in /proc ----------
vector<int> listPids() {
    vector<int> pids;
    DIR *dp = opendir("/proc");
    if (!dp) return pids;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            string name = entry->d_name;
            if (isDigits(name)) pids.push_back(stoi(name));
        }
    }
    closedir(dp);
    return pids;
}

// ---------- Clear console ----------
void clearScreen() {
    // ANSI escape: clear screen and move cursor to top-left
    cout << "\033[2J\033[H";
}

// ---------- Format uptime nicely ----------
string formatDuration(double seconds) {
    int s = (int)seconds;
    int days = s / 86400; s %= 86400;
    int hours = s / 3600; s %= 3600;
    int mins = s / 60; s %= 60;
    int secs = s;
    stringstream ss;
    if (days > 0) ss << days << "d ";
    ss << setfill('0') << setw(2) << hours << ":" << setw(2) << mins << ":" << setw(2) << secs;
    return ss.str();
}

// ---------- Main monitoring loop ----------
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    const int refresh_seconds = 2;
    const long clk_tck = sysconf(_SC_CLK_TCK); // clock ticks per second

    // previous samples
    CpuSample prevCpu = getCpuSample();
    ull prevTotal = prevCpu.total();
    ull prevIdleAll = prevCpu.idleAll();

    // previous per-process times map
    map<int, ull> prevProcTimes;

    while (true) {
        // sleep for refresh interval (but do it after computing initial values when loop starts)
        this_thread::sleep_for(chrono::seconds(refresh_seconds));

        CpuSample curCpu = getCpuSample();
        ull curTotal = curCpu.total();
        ull curIdleAll = curCpu.idleAll();

        ull totalDiff = (curTotal >= prevTotal) ? (curTotal - prevTotal) : 0;
        ull idleDiff = (curIdleAll >= prevIdleAll) ? (curIdleAll - prevIdleAll) : 0;

        double cpuUsagePercent = 0.0;
        if (totalDiff > 0) {
            cpuUsagePercent = (double)(totalDiff - idleDiff) * 100.0 / (double)totalDiff;
        }

        // memory
        MemInfo mem = getMemInfo();
        ull usedMem = 0;
        double memPercent = 0.0;
        if (mem.memTotal > 0) {
            // used = total - available (more robust than total - free)
            usedMem = (mem.memTotal > mem.available) ? (mem.memTotal - mem.available) : 0;
            memPercent = (double)usedMem * 100.0 / (double)mem.memTotal;
        }

        double uptime = getUptimeSeconds();

        // read processes
        vector<int> pids = listPids();
        vector<ProcessInfo> procs;
        procs.reserve(pids.size());

        for (int pid : pids) {
            ProcessInfo pi = readProcess(pid);
            // compute cpu percent per process using previous samples if available
            ull prevTime = 0;
            auto it = prevProcTimes.find(pid);
            if (it != prevProcTimes.end()) prevTime = it->second;
            // difference in process time in clock ticks
            ull diffProc = (pi.totalTime >= prevTime) ? (pi.totalTime - prevTime) : 0;
            double cpuPct = 0.0;
            if (totalDiff > 0) {
                // convert process ticks to same basis as totalDiff (totalDiff is in "jiffies" as read from /proc/stat)
                cpuPct = (double)diffProc * 100.0 / (double)totalDiff;
            }
            pi.cpuPercent = cpuPct;
            procs.push_back(pi);
            // update prev map for next round
            prevProcTimes[pid] = pi.totalTime;
        }

        // remove PIDs no longer present from prevProcTimes to keep map small
        vector<int> toRemove;
        for (auto &kv : prevProcTimes) {
            if (find(pids.begin(), pids.end(), kv.first) == pids.end()) toRemove.push_back(kv.first);
        }
        for (int r : toRemove) prevProcTimes.erase(r);

        // sort by cpu% descending then by mem descending
        sort(procs.begin(), procs.end(), [](const ProcessInfo &a, const ProcessInfo &b) {
            if (a.cpuPercent != b.cpuPercent) return a.cpuPercent > b.cpuPercent;
            return a.rss_kb > b.rss_kb;
        });

        // display
        clearScreen();
        cout << "==== Simple System Monitor ====\n";
        cout << "Uptime: " << formatDuration(uptime) << "    ";
        cout << "CPU Usage: " << fixed << setprecision(2) << cpuUsagePercent << "%    ";
        cout << "Memory: " << (usedMem / 1024) << " MB / " << (mem.memTotal / 1024) << " MB ";
        cout << "(" << fixed << setprecision(2) << memPercent << "%)\n";
        cout << "Refreshed every " << refresh_seconds << "s. (Press Ctrl+C to exit)\n\n";

        // header
        cout << left << setw(8) << "PID" << setw(25) << "NAME" << setw(10) << "CPU (%)" << setw(12) << "RSS (KB)" << "\n";
        cout << string(60, '-') << "\n";

        // show top 20 processes
        int shown = 0;
        for (const auto &p : procs) {
            if (shown++ >= 20) break;
            cout << left << setw(8) << p.pid;
            string name = p.name;
            if (name.size() > 23) name = name.substr(0, 20) + "...";
            cout << setw(25) << name;
            cout << setw(10) << fixed << setprecision(2) << p.cpuPercent;
            cout << setw(12) << p.rss_kb;
            cout << "\n";
        }

        // update prev cpu samples
        prevCpu = curCpu;
        prevTotal = curTotal;
        prevIdleAll = curIdleAll;
    }

    return 0;
}
