#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <termios.h>
#include <unistd.h>
#include <csignal>
#include <sys/ioctl.h>


struct ProcessInfo {
    int pid;
    std::string name;
    double cpuUsage;
    double ramUsage;
};

bool getProcessList(std::vector<ProcessInfo> &procs) {
    procs.clear();
    std::ifstream procStat("/proc/stat");
    if (!procStat.is_open()) return false;

    std::string cpuLine;
    std::getline(procStat, cpuLine);
    std::istringstream cpuStream(cpuLine);

    std::string cpu;
    long totalJiffies = 0;
    long user, nice, system, idle;
    cpuStream >> cpu >> user >> nice >> system >> idle;
    totalJiffies = user + nice + system + idle;

    for (int pid = 1; pid < 32768; pid++) {
        std::string statPath = "/proc/" + std::to_string(pid) + "/stat";
        std::ifstream statFile(statPath);
        if (!statFile.is_open()) continue;

        std::string comm;
        char state;
        long utime, stime;
        statFile >> pid >> comm >> state >> std::ws;
        for (int i = 0; i < 11; ++i) statFile >> std::ws;  // Skip to utime field
        statFile >> utime >> stime;

        double cpuUsage = (double)(utime + stime) / totalJiffies * 100.0;

        std::string statusPath = "/proc/" + std::to_string(pid) + "/status";
        std::ifstream statusFile(statusPath);
        if (!statusFile.is_open()) continue;

        std::string line;
        long ramUsage = 0;
        while (std::getline(statusFile, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream ramStream(line);
                std::string key;
                ramStream >> key >> ramUsage;
                break;
            }
        }

        procs.push_back({pid, comm.substr(1, comm.length() - 2), cpuUsage, (double)ramUsage});
    }
    return true;
}

void printProcessList(const std::vector<ProcessInfo> &procs) {
    std::cout << "\033[2J\033[1;1H";  // Clear screen
    std::cout << "PID     CPU%     RAM(KB)     NAME\n";
    for (const auto &proc : procs) {
        std::cout << proc.pid << "     " 
                  << proc.cpuUsage << "     " 
                  << proc.ramUsage << "     " 
                  << proc.name << "\n";
    }
}

char getNonBlockingInput() {
    char input = 0;
    struct termios old_tio, new_tio;

    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;

    new_tio.c_lflag &= (~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    int byteswaiting;
    ioctl(STDIN_FILENO, FIONREAD, &byteswaiting);

    if (byteswaiting > 0) {
        read(STDIN_FILENO, &input, 1);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    return input;
}

int main() {
    std::vector<ProcessInfo> procs;
    bool sortByCPU = true;
    int refreshInterval = 1;
    std::string filterString;

    while (true) {
        if (!getProcessList(procs)) {
            std::cerr << "Failed to read /proc.\n";
            return 1;
        }

        if (!filterString.empty()) {
            procs.erase(std::remove_if(procs.begin(), procs.end(),
                [&](const ProcessInfo &p) {
                    return p.name.find(filterString) == std::string::npos;
                }), procs.end());
        }

        if (sortByCPU) {
            std::sort(procs.begin(), procs.end(),
                [](const ProcessInfo &a, const ProcessInfo &b) {
                    return a.cpuUsage > b.cpuUsage;
                });
        } else {
            std::sort(procs.begin(), procs.end(),
                [](const ProcessInfo &a, const ProcessInfo &b) {
                    return a.ramUsage > b.ramUsage;
                });
        }

        printProcessList(procs);
        std::cout << "\n[q] Quit | [k] Kill PID | [s] Sort Toggle | [f] Filter | [+/-] Refresh: " 
                  << refreshInterval << "s\n";

        char c = getNonBlockingInput();
        if (c != 0) {
            if (c == 'q') break;
            else if (c == 's') sortByCPU = !sortByCPU;
            else if (c == '+') refreshInterval = std::min(refreshInterval + 1, 10);
            else if (c == '-') refreshInterval = std::max(refreshInterval - 1, 1);
            else if (c == 'f') {
                std::cout << "\nEnter filter substring: ";
                std::cin >> filterString;
            }
            else if (c == 'k') {
                int pid;
                std::cout << "\nEnter PID to kill: ";
                std::cin >> pid;
                if (kill(pid, SIGTERM) == 0) {
                    std::cout << "Process " << pid << " terminated.\n";
                } else {
                    std::perror("Failed to kill process");
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(refreshInterval));
    }

    return 0;
}
