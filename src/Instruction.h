#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

enum class InstructionType { PRINT, DECLARE, ADD, SUBTRACT, SLEEP, FOR, READ, WRITE };

// An instruction operand: a variable name or a uint16 literal.
struct Operand {
    bool isVariable = false;
    std::string variable;
    uint16_t value = 0;
};

// One process instruction. The fields used depend on the type:
//   PRINT    logs message, followed by the value of variable if one is given
//   DECLARE  variable = first.value
//   ADD      variable = first + second
//   SUBTRACT variable = first - second
//   SLEEP    gives up the CPU for sleepTicks ticks
//   FOR      runs body `repeats` times
//   READ     variable = the uint16 stored at address
//   WRITE    stores first as a uint16 at address
struct Instruction {
    InstructionType type = InstructionType::PRINT;
    std::string message;
    std::string variable;
    Operand first;
    Operand second;
    uint8_t sleepTicks = 0;
    uint16_t repeats = 0;
    std::vector<Instruction> body;
    uint64_t address = 0; // a virtual address in the process's memory
};

// FOR loops nest at most this deep.
constexpr int kMaxForNesting = 3;

// Number of instructions a statement executes: 1, or for a FOR, its body's count times repeats.
uint64_t executedCount(const Instruction& statement);

// Generates a random statement that executes between 1 and maxCount instructions, for a process
// with memoryBytes (at least 64) of memory. depth is the number of FOR loops around it; a FOR is only
// generated below kMaxForNesting. READ and WRITE use even addresses past the 64-byte symbol table,
// or anywhere in a 64-byte process.
Instruction randomStatement(std::minstd_rand& rng, const std::string& processName, uint64_t memoryBytes, int depth,
                            uint64_t maxCount);
