#include "Process.h"

#include <algorithm>
#include <utility>

#include "MemoryManager.h"

namespace {

uint16_t clampToUint16(int64_t value) {
    return static_cast<uint16_t>(std::clamp<int64_t>(value, 0, UINT16_MAX));
}

} // namespace

Process::Process(int id, std::string name, uint64_t memoryBytes, uint64_t totalInstructions, uint32_t seed)
    : id(id), name(std::move(name)), createdAt(std::time(nullptr)), memoryBytes(memoryBytes),
      totalInstructions(totalInstructions), rng(seed) {}

Process::Process(int id, std::string name, uint64_t memoryBytes, std::vector<Instruction> program)
    : id(id), name(std::move(name)), createdAt(std::time(nullptr)), memoryBytes(memoryBytes), fromProgram(true),
      program(std::move(program)) {
    for (const Instruction& statement : this->program) {
        totalInstructions += executedCount(statement);
    }
}

const Instruction& Process::peek() {
    if (pending == nullptr) {
        pending = &nextInstruction();
    }
    return *pending;
}

std::vector<uint64_t> Process::pagesFor(const Instruction& instruction, uint64_t frameBytes) const {
    bool usesVariable = false;
    bool usesAddress = false;
    switch (instruction.type) {
    case InstructionType::PRINT:
        usesVariable = !instruction.variable.empty();
        break;
    case InstructionType::DECLARE:
    case InstructionType::ADD:
    case InstructionType::SUBTRACT:
        usesVariable = true;
        break;
    case InstructionType::READ:
        usesVariable = true;
        usesAddress = true;
        break;
    case InstructionType::WRITE:
        usesVariable = instruction.first.isVariable;
        usesAddress = true;
        break;
    case InstructionType::SLEEP:
    case InstructionType::FOR:
        break;
    }

    std::vector<uint64_t> pages;
    if (usesVariable) {
        pages.push_back(0);
    }
    if (usesAddress && isValidAddress(instruction.address)) {
        // The uint16 is at address and address + 1, which may be on two pages.
        for (uint64_t page : {instruction.address / frameBytes, (instruction.address + 1) / frameBytes}) {
            if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
                pages.push_back(page);
            }
        }
    }
    return pages;
}

StepResult Process::step(int core, MemoryManager& memory) {
    const Instruction& instruction = peek();
    pending = nullptr;
    if (!execute(instruction, core, memory)) {
        crashed = true;
        crashTime = std::time(nullptr);
        crashAddress = instruction.address;
        releaseState();
        return StepResult::CRASHED;
    }
    ++executed;
    if (executed >= totalInstructions) {
        releaseState();
        return StepResult::FINISHED;
    }
    return instruction.type == InstructionType::SLEEP ? StepResult::SLEPT : StepResult::RAN;
}

bool Process::execute(const Instruction& instruction, int core, MemoryManager& memory) {
    switch (instruction.type) {
    case InstructionType::PRINT: {
        std::string text = instruction.message;
        if (!instruction.variable.empty()) {
            text += std::to_string(load(instruction.variable, memory));
        }
        logs.push_back({std::time(nullptr), core, std::move(text)});
        break;
    }
    case InstructionType::DECLARE:
        store(instruction.variable, instruction.first.value, memory);
        break;
    case InstructionType::ADD: {
        uint16_t sum = clampToUint16(int64_t{read(instruction.first, memory)} + read(instruction.second, memory));
        store(instruction.variable, sum, memory);
        break;
    }
    case InstructionType::SUBTRACT: {
        uint16_t difference =
            clampToUint16(int64_t{read(instruction.first, memory)} - read(instruction.second, memory));
        store(instruction.variable, difference, memory);
        break;
    }
    case InstructionType::SLEEP:
        sleepTicks = instruction.sleepTicks;
        break;
    case InstructionType::READ:
        if (!isValidAddress(instruction.address)) {
            return false;
        }
        store(instruction.variable, memory.read16(id, instruction.address), memory);
        break;
    case InstructionType::WRITE:
        if (!isValidAddress(instruction.address)) {
            return false;
        }
        memory.write16(id, instruction.address, read(instruction.first, memory));
        break;
    case InstructionType::FOR:
        break; // nextInstruction() enters loops itself
    }
    return true;
}

std::optional<uint16_t> Process::variable(const std::string& name, const MemoryManager& memory) const {
    auto found = std::find(symbols.begin(), symbols.end(), name);
    if (found == symbols.end()) {
        return std::nullopt;
    }
    return memory.read16(id, 2 * static_cast<uint64_t>(found - symbols.begin()));
}

size_t Process::variableCount() const {
    return symbols.size();
}

int Process::getId() const {
    return id;
}

const std::string& Process::getName() const {
    return name;
}

std::time_t Process::getCreatedAt() const {
    return createdAt;
}

uint64_t Process::getMemoryBytes() const {
    return memoryBytes;
}

uint64_t Process::getExecuted() const {
    return executed;
}

uint64_t Process::getTotalInstructions() const {
    return totalInstructions;
}

bool Process::isFinished() const {
    return crashed || executed >= totalInstructions;
}

bool Process::hasCrashed() const {
    return crashed;
}

std::time_t Process::crashedAt() const {
    return crashTime;
}

uint64_t Process::faultAddress() const {
    return crashAddress;
}

const std::vector<LogEntry>& Process::getLogs() const {
    return logs;
}

uint8_t Process::getSleepTicks() const {
    return sleepTicks;
}

const Instruction& Process::nextInstruction() {
    while (true) {
        if (loops.empty()) {
            if (fromProgram) {
                // Finished processes are never stepped, so a statement is always left here.
                const Instruction& next = program[nextStatement++];
                if (next.type != InstructionType::FOR) {
                    return next;
                }
                enterLoop(next);
            } else {
                statement = randomStatement(rng, name, memoryBytes, 0, totalInstructions - executed);
                if (statement.type != InstructionType::FOR) {
                    return statement;
                }
                enterLoop(statement);
            }
            continue;
        }

        LoopFrame& frame = loops.back();
        if (frame.next == frame.loop->body.size()) {
            // End of the body: run it again or leave the loop.
            if (--frame.repeatsLeft > 0) {
                frame.next = 0;
            } else {
                loops.pop_back();
            }
            continue;
        }

        const Instruction& instruction = frame.loop->body[frame.next++];
        if (instruction.type != InstructionType::FOR) {
            return instruction;
        }
        enterLoop(instruction);
    }
}

void Process::enterLoop(const Instruction& loop) {
    // A loop that runs nothing (no repeats or an empty body) is skipped.
    if (executedCount(loop) > 0) {
        loops.push_back({&loop, 0, loop.repeats});
    }
}

bool Process::isValidAddress(uint64_t address) const {
    // The whole uint16, at address and address + 1, must be inside the process's memory.
    return memoryBytes >= 2 && address <= memoryBytes - 2;
}

std::optional<size_t> Process::slotOf(const std::string& variable, MemoryManager& memory) {
    auto found = std::find(symbols.begin(), symbols.end(), variable);
    if (found != symbols.end()) {
        return static_cast<size_t>(found - symbols.begin());
    }
    if (symbols.size() >= kMaxVariables) {
        return std::nullopt; // the symbol table is full: the declaration is ignored
    }
    symbols.push_back(variable);
    size_t slot = symbols.size() - 1;
    memory.write16(id, 2 * static_cast<uint64_t>(slot), 0); // a new variable starts at 0
    return slot;
}

uint16_t Process::load(const std::string& variable, MemoryManager& memory) {
    // Variables used before being declared start at 0.
    std::optional<size_t> slot = slotOf(variable, memory);
    return slot ? memory.read16(id, 2 * static_cast<uint64_t>(*slot)) : 0;
}

void Process::store(const std::string& variable, uint16_t value, MemoryManager& memory) {
    if (std::optional<size_t> slot = slotOf(variable, memory)) {
        memory.write16(id, 2 * static_cast<uint64_t>(*slot), value);
    }
}

uint16_t Process::read(const Operand& operand, MemoryManager& memory) {
    return operand.isVariable ? load(operand.variable, memory) : operand.value;
}

void Process::releaseState() {
    // A finished process no longer needs its variables, loops or statements; the scheduler frees
    // its memory.
    symbols.clear();
    loops.clear();
    program.clear();
    statement = Instruction();
    pending = nullptr;
}
