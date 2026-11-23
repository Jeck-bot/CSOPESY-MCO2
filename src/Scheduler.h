#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Config.h"
#include "Instruction.h"
#include "MemoryManager.h"
#include "Process.h"

// scheduler-start counts batch-process-freq in batch cycles of this many CPU ticks (200 ms), so the
// cores run faster than processes arrive. The quiz's expected outputs need this: Q3 (8 cores,
// batch-process-freq 1, 100 instructions) must end mostly finished, and Q4 (1 core, X in [10, 30],
// delay Y in [1, 5]) must go idle between processes. Counting in raw ticks makes both impossible.
constexpr uint64_t kTicksPerBatchCycle = 20;

// One row of screen -ls: a process on a core, or a finished process (core is -1). A process that
// crashed shows the instructions it executed before the crash.
struct ProcessRow {
    std::string name;
    std::time_t createdAt = 0;
    int core = -1;
    uint64_t executed = 0;
    uint64_t total = 0;
};

// What screen -ls and report-util show.
struct Utilization {
    uint32_t coresUsed = 0;
    uint32_t coreCount = 0;
    std::vector<ProcessRow> running;  // processes on cores, in core order
    std::vector<ProcessRow> finished; // in the order they finished
};

// What a process's screen shows.
struct ProcessView {
    std::string name;
    int id = 0;
    uint64_t memoryBytes = 0;
    std::vector<LogEntry> logs;
    uint64_t executed = 0;
    uint64_t total = 0;
    bool finished = false;      // ran every instruction or crashed
    bool crashed = false;       // shut down by a memory access violation
    std::time_t crashedAt = 0;  // when it crashed
    uint64_t faultAddress = 0;  // the invalid address
};

// One process in the main menu's process-smi.
struct ProcessMemory {
    std::string name;
    uint64_t bytes = 0; // bytes of the frames it holds
};

// What the main menu's process-smi and vmstat show.
struct MemoryReport {
    uint32_t coresUsed = 0;
    uint32_t coreCount = 0;
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;               // bytes of all frames in use
    std::vector<ProcessMemory> processes; // unfinished processes holding frames, in ID order
    uint64_t idleTicks = 0;               // core-ticks without a process
    uint64_t activeTicks = 0;             // core-ticks with a process
    uint64_t pagedIn = 0;
    uint64_t pagedOut = 0;
};

// Real-time CPU scheduler. A background thread runs one CPU tick every 10 ms on all cores.
//
// Memory: a process's page table is made when it first gets a core. Before each instruction, the
// core checks that the pages it needs are loaded. If not, that tick is a page fault: the pages are
// loaded (evicting others to the backing store if needed) and pinned, and the instruction runs on
// a later tick. Finished and crashed processes free their memory.
class Scheduler {
public:
    Scheduler(const Config& config, uint64_t seed, std::string backingStorePath = "csopesy-backing-store.txt");
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Starts and stops the background thread that calls tick().
    void start();
    void stop();

    // One CPU cycle: run every busy core, generate a batch process when due, wake sleepers whose
    // sleep is over, and give idle cores the next ready processes.
    void tick();

    // screen -s: creates a process with memoryBytes of memory and a random length in
    // [min-ins, max-ins], and queues it. Returns false if the name is taken.
    bool createProcess(const std::string& name, uint64_t memoryBytes);

    // screen -c: creates a process that runs program, and queues it. Returns false if the name is taken.
    bool createProcess(const std::string& name, uint64_t memoryBytes, std::vector<Instruction> program);

    // A random power of 2 in [min-mem-per-proc, max-mem-per-proc].
    uint64_t rollMemorySize();

    // The process with this name, or nullopt if there is none.
    std::optional<ProcessView> view(const std::string& name) const;

    // scheduler-start / scheduler-stop. Return false if generation was already on / off.
    bool startBatchGeneration();
    bool stopBatchGeneration();

    Utilization utilization() const;
    MemoryReport memoryReport() const;

private:
    struct CpuCore {
        std::shared_ptr<Process> process; // null when idle
        uint64_t quantumUsed = 0;         // ticks since this process was dispatched
    };

    // A process with a random length in [min-ins, max-ins].
    std::shared_ptr<Process> generatedProcess(const std::string& name, uint64_t memoryBytes);
    void addProcess(std::shared_ptr<Process> process);
    uint64_t rollMemorySizeLocked();
    std::string nextBatchName();

    Config config;
    MemoryManager memory;
    std::mt19937_64 rng;
    std::vector<CpuCore> cores;
    std::deque<std::shared_ptr<Process>> readyQueue;
    std::multimap<uint64_t, std::shared_ptr<Process>> sleeping; // keyed by the tick they wake
    std::vector<std::shared_ptr<Process>> finished;              // in the order they finished
    std::unordered_map<std::string, std::shared_ptr<Process>> processes;
    uint64_t cpuTicks = 0;
    uint64_t idleTicks = 0;
    uint64_t activeTicks = 0;
    uint64_t ticksSinceBatch = 0;
    int nextProcessId = 1;
    int nextBatchNumber = 1;
    bool generatingBatch = false;

    std::atomic<bool> running{false};
    std::thread tickThread;
    mutable std::mutex mutex;
};
