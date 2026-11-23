#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "Instruction.h"

class MemoryManager;

// What happened when a process ran one instruction.
enum class StepResult { RAN, SLEPT, FINISHED, CRASHED };

// One PRINT output. process-smi shows it as: (time) Core:N "text"
struct LogEntry {
    std::time_t time = 0;
    int core = 0;
    std::string text;
};

// The symbol table is the first 64 bytes of a process's memory. Variable i is the uint16 at
// address 2i, so it holds 32 variables.
constexpr uint64_t kSymbolTableBytes = 64;
constexpr size_t kMaxVariables = kSymbolTableBytes / 2;

// A process with memoryBytes of emulated memory. Its variables and READ/WRITE data live in that
// memory, which the scheduler pages in through a MemoryManager before each instruction.
class Process {
public:
    // A process whose totalInstructions instructions are generated from its seed as it runs
    // (screen -s and scheduler-start).
    Process(int id, std::string name, uint64_t memoryBytes, uint64_t totalInstructions, uint32_t seed);

    // A process that runs the given statements (screen -c).
    Process(int id, std::string name, uint64_t memoryBytes, std::vector<Instruction> program);

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // The next instruction to run, never a FOR. It stays the same until step() runs it.
    const Instruction& peek();

    // The pages of this process an instruction touches: page 0, the symbol table, if it uses a
    // variable, and the pages holding a valid READ or WRITE address.
    std::vector<uint64_t> pagesFor(const Instruction& instruction, uint64_t frameBytes) const;

    // Runs the next instruction on core; the pages it needs (pagesFor) must be resident. Returns
    // SLEPT after a SLEEP (see getSleepTicks()), FINISHED after the last instruction, and CRASHED
    // when it accessed an address outside the process's memory, which shuts the process down.
    StepResult step(int core, MemoryManager& memory);

    // Executes one instruction other than FOR. Returns false on a memory access violation.
    bool execute(const Instruction& instruction, int core, MemoryManager& memory);

    // The value of a variable in the symbol table, or nullopt if it isn't there. Page 0 must be
    // resident.
    std::optional<uint16_t> variable(const std::string& name, const MemoryManager& memory) const;
    size_t variableCount() const;

    int getId() const;
    const std::string& getName() const;
    std::time_t getCreatedAt() const;
    uint64_t getMemoryBytes() const;
    uint64_t getExecuted() const;          // "Current instruction line"
    uint64_t getTotalInstructions() const; // "Lines of code"
    bool isFinished() const;               // ran every instruction or crashed
    bool hasCrashed() const;
    std::time_t crashedAt() const;         // when the access violation happened
    uint64_t faultAddress() const;         // the invalid address
    const std::vector<LogEntry>& getLogs() const;
    uint8_t getSleepTicks() const;         // ticks requested by the last SLEEP

    // Busy-wait ticks left before the next instruction (delay-per-exec). Managed by the scheduler.
    uint64_t delayLeft = 0;

private:
    // Position inside an active FOR loop.
    struct LoopFrame {
        const Instruction* loop;
        size_t next;          // index of the next body instruction
        uint16_t repeatsLeft; // repetitions left, including the current one
    };

    const Instruction& nextInstruction();
    void enterLoop(const Instruction& loop);
    bool isValidAddress(uint64_t address) const;
    std::optional<size_t> slotOf(const std::string& variable, MemoryManager& memory);
    uint16_t load(const std::string& variable, MemoryManager& memory);
    void store(const std::string& variable, uint16_t value, MemoryManager& memory);
    uint16_t read(const Operand& operand, MemoryManager& memory);
    void releaseState();

    int id;
    std::string name;
    std::time_t createdAt;
    uint64_t memoryBytes;
    uint64_t totalInstructions = 0;
    uint64_t executed = 0;
    std::minstd_rand rng;
    bool fromProgram = false;
    std::vector<Instruction> program;  // screen -c statements
    size_t nextStatement = 0;          // the program statement to start next
    Instruction statement;             // the generated top-level statement being run
    std::vector<LoopFrame> loops;      // active FOR loops, innermost last
    const Instruction* pending = nullptr; // the instruction peek() chose, until step() runs it
    std::vector<std::string> symbols;  // variable names; symbols[i] is stored at address 2i
    std::vector<LogEntry> logs;
    uint8_t sleepTicks = 0;
    bool crashed = false;
    std::time_t crashTime = 0;
    uint64_t crashAddress = 0;
};
