#include "Scheduler.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <utility>

namespace {

constexpr std::chrono::milliseconds kTickPeriod(10);

ProcessRow rowFor(const Process& process, int core) {
    return {process.getName(), process.getCreatedAt(), core, process.getExecuted(), process.getTotalInstructions()};
}

} // namespace

Scheduler::Scheduler(const Config& config, uint64_t seed, std::string backingStorePath)
    : config(config), memory(config.maxOverallMem, config.memPerFrame, std::move(backingStorePath)), rng(seed),
      cores(config.numCpu) {}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    if (running.exchange(true)) {
        return;
    }
    tickThread = std::thread([this] {
        auto nextTick = std::chrono::steady_clock::now();
        while (running) {
            tick();
            nextTick += kTickPeriod;
            auto now = std::chrono::steady_clock::now();
            if (nextTick < now) {
                nextTick = now; // fell behind: carry on from now instead of rushing to catch up
            }
            std::this_thread::sleep_until(nextTick);
        }
    });
}

void Scheduler::stop() {
    running = false;
    if (tickThread.joinable()) {
        tickThread.join();
    }
}

void Scheduler::tick() {
    std::lock_guard<std::mutex> lock(mutex);

    for (size_t i = 0; i < cores.size(); ++i) {
        CpuCore& core = cores[i];
        if (!core.process) {
            ++idleTicks;
            continue;
        }
        ++activeTicks;
        Process& process = *core.process;
        int id = process.getId();

        if (process.delayLeft > 0) {
            --process.delayLeft; // busy-waiting: the process keeps the core
        } else {
            std::vector<uint64_t> pages = process.pagesFor(process.peek(), memory.frameBytes());
            bool loaded = std::all_of(pages.begin(), pages.end(),
                                      [this, id](uint64_t page) { return memory.isResident(id, page); });
            if (!loaded) {
                // Page fault: load the pages this tick (if enough frames can be freed) and run the
                // instruction on a later tick. Loaded pages stay pinned until it runs.
                memory.pageIn(id, pages);
            } else {
                StepResult result = process.step(static_cast<int>(i), memory);
                memory.unpin(id);
                process.delayLeft = config.delayPerExec;
                if (result == StepResult::FINISHED || result == StepResult::CRASHED) {
                    memory.release(id);
                    finished.push_back(std::move(core.process));
                    continue;
                }
                if (result == StepResult::SLEPT) {
                    // Off the CPU for sleepTicks ticks, then back to the ready queue.
                    sleeping.emplace(cpuTicks + process.getSleepTicks() + 1, std::move(core.process));
                    continue;
                }
            }
        }

        ++core.quantumUsed;
        if (config.scheduler == SchedulerType::RR && core.quantumUsed >= config.quantumCycles && !readyQueue.empty()) {
            readyQueue.push_back(std::move(core.process));
        }
    }

    if (generatingBatch && ++ticksSinceBatch >= config.batchProcessFreq * kTicksPerBatchCycle) {
        ticksSinceBatch = 0;
        std::string name = nextBatchName();
        addProcess(generatedProcess(name, rollMemorySizeLocked()));
    }

    // Wake processes that may run again from the next tick.
    while (!sleeping.empty() && sleeping.begin()->first <= cpuTicks + 1) {
        readyQueue.push_back(std::move(sleeping.begin()->second));
        sleeping.erase(sleeping.begin());
    }

    for (CpuCore& core : cores) {
        if (!core.process && !readyQueue.empty()) {
            core.process = std::move(readyQueue.front());
            readyQueue.pop_front();
            core.quantumUsed = 0;
            // A process gets its memory the first time it is on a core.
            memory.allocate(core.process->getId(), core.process->getName(), core.process->getMemoryBytes());
        }
    }

    if (cpuTicks % kTicksPerBackingStoreWrite == 0) {
        memory.flush();
    }
    ++cpuTicks;
}

bool Scheduler::createProcess(const std::string& name, uint64_t memoryBytes) {
    std::lock_guard<std::mutex> lock(mutex);
    if (processes.count(name) > 0) {
        return false;
    }
    addProcess(generatedProcess(name, memoryBytes));
    return true;
}

bool Scheduler::createProcess(const std::string& name, uint64_t memoryBytes, std::vector<Instruction> program) {
    std::lock_guard<std::mutex> lock(mutex);
    if (processes.count(name) > 0) {
        return false;
    }
    addProcess(std::make_shared<Process>(nextProcessId++, name, memoryBytes, std::move(program)));
    return true;
}

uint64_t Scheduler::rollMemorySize() {
    std::lock_guard<std::mutex> lock(mutex);
    return rollMemorySizeLocked();
}

std::optional<ProcessView> Scheduler::view(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = processes.find(name);
    if (found == processes.end()) {
        return std::nullopt;
    }
    const Process& process = *found->second;
    ProcessView view;
    view.name = process.getName();
    view.id = process.getId();
    view.memoryBytes = process.getMemoryBytes();
    view.logs = process.getLogs();
    view.executed = process.getExecuted();
    view.total = process.getTotalInstructions();
    view.finished = process.isFinished();
    view.crashed = process.hasCrashed();
    view.crashedAt = process.crashedAt();
    view.faultAddress = process.faultAddress();
    return view;
}

bool Scheduler::startBatchGeneration() {
    std::lock_guard<std::mutex> lock(mutex);
    if (generatingBatch) {
        return false;
    }
    generatingBatch = true;
    ticksSinceBatch = 0;
    return true;
}

bool Scheduler::stopBatchGeneration() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!generatingBatch) {
        return false;
    }
    generatingBatch = false;
    return true;
}

Utilization Scheduler::utilization() const {
    std::lock_guard<std::mutex> lock(mutex);
    Utilization usage;
    usage.coreCount = static_cast<uint32_t>(cores.size());
    for (size_t i = 0; i < cores.size(); ++i) {
        if (cores[i].process) {
            ++usage.coresUsed;
            usage.running.push_back(rowFor(*cores[i].process, static_cast<int>(i)));
        }
    }
    for (const std::shared_ptr<Process>& process : finished) {
        usage.finished.push_back(rowFor(*process, -1));
    }
    return usage;
}

MemoryReport Scheduler::memoryReport() const {
    std::lock_guard<std::mutex> lock(mutex);
    MemoryReport report;
    report.coreCount = static_cast<uint32_t>(cores.size());
    for (const CpuCore& core : cores) {
        if (core.process) {
            ++report.coresUsed;
        }
    }
    report.totalBytes = memory.totalBytes();
    report.usedBytes = memory.usedBytes();

    std::vector<std::pair<int, ProcessMemory>> holders;
    for (const auto& [name, process] : processes) {
        uint64_t bytes = memory.residentBytes(process->getId());
        if (!process->isFinished() && bytes > 0) {
            holders.push_back({process->getId(), ProcessMemory{name, bytes}});
        }
    }
    std::sort(holders.begin(), holders.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    for (auto& holder : holders) {
        report.processes.push_back(std::move(holder.second));
    }

    report.idleTicks = idleTicks;
    report.activeTicks = activeTicks;
    report.pagedIn = memory.pagedIn();
    report.pagedOut = memory.pagedOut();
    return report;
}

std::shared_ptr<Process> Scheduler::generatedProcess(const std::string& name, uint64_t memoryBytes) {
    uint64_t length = std::uniform_int_distribution<uint64_t>(config.minIns, config.maxIns)(rng);
    uint32_t seed = static_cast<uint32_t>(rng());
    return std::make_shared<Process>(nextProcessId++, name, memoryBytes, length, seed);
}

void Scheduler::addProcess(std::shared_ptr<Process> process) {
    processes.emplace(process->getName(), process);
    readyQueue.push_back(std::move(process));
}

uint64_t Scheduler::rollMemorySizeLocked() {
    // Memory sizes are powers of 2, so roll the exponent.
    int low = std::countr_zero(config.minMemPerProc);
    int high = std::countr_zero(config.maxMemPerProc);
    return uint64_t{1} << std::uniform_int_distribution<int>(low, high)(rng);
}

std::string Scheduler::nextBatchName() {
    // p01, p02, ..., p99, p100, ...; skips names already used by "screen -s".
    while (true) {
        std::string number = std::to_string(nextBatchNumber++);
        std::string name = "p" + std::string(number.size() < 2 ? "0" : "") + number;
        if (processes.count(name) == 0) {
            return name;
        }
    }
}
