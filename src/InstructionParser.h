#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Instruction.h"

// A "screen -c" program has this many top-level statements at most (and at least one).
constexpr size_t kMaxProgramStatements = 50;

// The parts of: screen -c <process name> [<memory size>] "<instructions>"
struct ScreenCreateArgs {
    std::string name;
    std::string sizeText;     // empty when no memory size was given
    std::string instructions; // the text between the quotes, with each \" turned into "
};

// Splits the text after "screen -c" into its parts. Returns nullopt if the name or the quoted
// instructions are missing, or if anything follows the closing quote.
std::optional<ScreenCreateArgs> parseScreenCreate(const std::string& text);

// Parses 1 to kMaxProgramStatements semicolon-separated statements. Keywords ignore case. Each
// statement can be written with spaces or in parentheses:
//   DECLARE var 10           DECLARE(var, 10)
//   ADD var a b              ADD(var, a, b)          (also SUBTRACT; a and b are variables or numbers)
//   SLEEP 3                  SLEEP(3)
//   READ var 0x500           READ(var, 0x500)        (addresses are hex, with or without 0x)
//   WRITE 0x500 a            WRITE(0x500, a)
//   PRINT("text"), PRINT(var), PRINT("text" + var)
//   FOR([statement; statement; ...], repeats)        (nested at most kMaxForNesting deep)
// Numbers are clamped to [0, 65535] (SLEEP to [0, 255]). Returns nullopt if any statement is invalid
// or the number of statements is out of range.
std::optional<std::vector<Instruction>> parseProgram(const std::string& text);
