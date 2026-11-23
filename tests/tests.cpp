// Unit tests for the CSOPESY emulator.
// Usage, from the repo root: msbuild tests\CSOPESY.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64,
// then run tests\x64\Debug\CSOPESY.Tests.exe. Expect "0 failures".

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Included by path: putting src/ on the include path would make <thread>'s <process.h> resolve to
// Process.h, since Windows file names are case-insensitive.
#include "../src/BackingStore.h"
#include "../src/Config.h"
#include "../src/Instruction.h"
#include "../src/InstructionParser.h"
#include "../src/MemoryManager.h"
#include "../src/Process.h"
#include "../src/Scheduler.h"
#include "../src/Utils.h"

namespace {

int checks = 0;
int failures = 0;
std::string currentTest;

void check(bool ok, const char* text, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "FAIL [" << currentTest << "] line " << line << ": " << text << "\n";
    }
}

struct Test {
    const char* name;
    void (*run)();
};

std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

bool registerTest(const char* name, void (*run)()) {
    registry().push_back({name, run});
    return true;
}

// Local time as a time_t, so timestamp tests don't depend on the machine's time zone.
std::time_t localTime(int year, int month, int day, int hour, int minute, int second) {
    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
    parts.tm_isdst = -1;
    return std::mktime(&parts);
}

}  // namespace

#define CHECK(condition) check((condition), #condition, __LINE__)
#define TEST(name)                                                  \
    void name();                                                    \
    static const bool name##Registered = registerTest(#name, name); \
    void name()

// ---- Utils ----

TEST(formatsMorningTimeWithAm) {
    CHECK(formatTimestamp(localTime(2024, 1, 18, 9, 15, 22)) == "01/18/2024 09:15:22AM");
}

TEST(formatsEveningTimeWithPm) {
    CHECK(formatTimestamp(localTime(2024, 8, 6, 21, 5, 9)) == "08/06/2024 09:05:09PM");
}

TEST(formatsMidnightAsTwelveAm) {
    CHECK(formatTimestamp(localTime(2024, 12, 31, 0, 0, 0)) == "12/31/2024 12:00:00AM");
}

TEST(formatsNoonAsTwelvePm) {
    CHECK(formatTimestamp(localTime(2025, 3, 1, 12, 0, 7)) == "03/01/2025 12:00:07PM");
}

TEST(trimRemovesSurroundingWhitespace) {
    CHECK(trim("  screen -ls \t\r\n") == "screen -ls");
    CHECK(trim("   ") == "");
    CHECK(trim("") == "");
}

TEST(splitWordsIgnoresExtraSpaces) {
    std::vector<std::string> words = splitWords("  screen   -s  p01 ");
    CHECK(words.size() == 3);
    CHECK(words.size() == 3 && words[0] == "screen" && words[1] == "-s" && words[2] == "p01");
    CHECK(splitWords("   ").empty());
}

TEST(toLowerLowercasesLetters) {
    CHECK(toLower("Scheduler-START") == "scheduler-start");
}

// ---- Config ----

namespace {

bool parse(const std::string& text, Config& config, std::string& error) {
    std::istringstream input(text);
    return parseConfig(input, config, error);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(parsesSpecSampleConfig) {
    Config config;
    std::string error;
    CHECK(parse("num-cpu 4\nscheduler \"rr\"\nquantum-cycles 5\nbatch-process-freq 1\n"
                "min-ins 1000\nmax-ins 2000\ndelay-per-exec 0\n",
                config, error));
    CHECK(config.numCpu == 4);
    CHECK(config.scheduler == SchedulerType::RR);
    CHECK(config.quantumCycles == 5);
    CHECK(config.batchProcessFreq == 1);
    CHECK(config.minIns == 1000);
    CHECK(config.maxIns == 2000);
    CHECK(config.delayPerExec == 0);
}

TEST(parsesConfigWrittenOnOneLine) {
    Config config;
    std::string error;
    CHECK(parse("num-cpu 8 scheduler fcfs quantum-cycles 3 batch-process-freq 7 min-ins 10 max-ins 20 "
                "delay-per-exec 2",
                config, error));
    CHECK(config.numCpu == 8);
    CHECK(config.scheduler == SchedulerType::FCFS);
    CHECK(config.quantumCycles == 3);
    CHECK(config.batchProcessFreq == 7);
    CHECK(config.minIns == 10);
    CHECK(config.maxIns == 20);
    CHECK(config.delayPerExec == 2);
}

TEST(acceptsSchedulerInAnyCaseWithOrWithoutQuotes) {
    Config config;
    std::string error;
    CHECK(parse("scheduler \"FCFS\"", config, error));
    CHECK(config.scheduler == SchedulerType::FCFS);
    CHECK(parse("scheduler RR", config, error));
    CHECK(config.scheduler == SchedulerType::RR);
}

TEST(acceptsDelaysPerExecSpelling) {
    Config config;
    std::string error;
    CHECK(parse("delays-per-exec 7", config, error));
    CHECK(config.delayPerExec == 7);
}

TEST(usesSpecDefaultsForMissingKeys) {
    Config config;
    config.numCpu = 99;
    std::string error;
    CHECK(parse("min-ins 1", config, error));
    CHECK(config.numCpu == 4);
    CHECK(config.scheduler == SchedulerType::RR);
    CHECK(config.quantumCycles == 5);
    CHECK(config.batchProcessFreq == 1);
    CHECK(config.minIns == 1);
    CHECK(config.maxIns == 2000);
    CHECK(config.delayPerExec == 0);
}

TEST(acceptsRangeLimits) {
    Config config;
    std::string error;
    CHECK(parse("num-cpu 1", config, error));
    CHECK(parse("num-cpu 128", config, error));
    CHECK(parse("delay-per-exec 0", config, error));
    CHECK(parse("quantum-cycles 4294967296 batch-process-freq 4294967296 min-ins 4294967296 "
                "max-ins 4294967296 delay-per-exec 4294967296",
                config, error));
    CHECK(config.quantumCycles == 4294967296ULL);
    CHECK(config.batchProcessFreq == 4294967296ULL);
    CHECK(config.minIns == 4294967296ULL);
    CHECK(config.maxIns == 4294967296ULL);
    CHECK(config.delayPerExec == 4294967296ULL);
}

TEST(rejectsValuesOutsideTheRanges) {
    Config config;
    std::string error;
    CHECK(!parse("num-cpu 0", config, error));
    CHECK(!parse("num-cpu 129", config, error));
    CHECK(!parse("quantum-cycles 0", config, error));
    CHECK(!parse("quantum-cycles 4294967297", config, error));
    CHECK(!parse("batch-process-freq 0", config, error));
    CHECK(!parse("min-ins 0", config, error));
    CHECK(!parse("max-ins 4294967297", config, error));
    CHECK(!parse("delay-per-exec 4294967297", config, error));
}

TEST(rejectsNonNumericValues) {
    Config config;
    std::string error;
    CHECK(!parse("num-cpu four", config, error));
    CHECK(!parse("num-cpu -1", config, error));
    CHECK(!parse("min-ins 12abc", config, error));
    CHECK(!parse("max-ins 99999999999999999999999", config, error));
}

TEST(rejectsUnknownScheduler) {
    Config config;
    std::string error;
    CHECK(!parse("scheduler sjf", config, error));
    CHECK(contains(error, "scheduler"));
}

TEST(rejectsMinInsAboveMaxIns) {
    Config config;
    std::string error;
    CHECK(!parse("min-ins 10 max-ins 5", config, error));
    CHECK(contains(error, "min-ins"));
}

TEST(rejectsKeyWithoutValue) {
    Config config;
    std::string error;
    CHECK(!parse("num-cpu 4 scheduler", config, error));
    CHECK(contains(error, "scheduler"));
}

TEST(errorNamesTheBadKey) {
    Config config;
    std::string error;
    CHECK(!parse("num-cpu 0", config, error));
    CHECK(contains(error, "num-cpu"));
}

TEST(leavesConfigUnchangedOnError) {
    Config config;
    config.numCpu = 7;
    std::string error;
    CHECK(!parse("num-cpu 2 max-ins 0", config, error));
    CHECK(config.numCpu == 7);
}

TEST(ignoresUnknownKeys) {
    Config config;
    std::string error;
    CHECK(parse("colour blue num-cpu 2", config, error));
    CHECK(config.numCpu == 2);
}

TEST(loadConfigReadsAFile) {
    {
        std::ofstream file("test-config.txt");
        file << "num-cpu 3\nscheduler \"fcfs\"\n";
    }
    Config config;
    std::string error;
    CHECK(loadConfig("test-config.txt", config, error));
    CHECK(config.numCpu == 3);
    CHECK(config.scheduler == SchedulerType::FCFS);
    std::remove("test-config.txt");
}

TEST(loadConfigReportsMissingFile) {
    Config config;
    std::string error;
    CHECK(!loadConfig("no-such-config-file.txt", config, error));
    CHECK(contains(error, "no-such-config-file.txt"));
}

TEST(parsesMemoryParameters) {
    Config config;
    std::string error;
    CHECK(parse("num-cpu 4\nscheduler \"rr\"\nquantum-cycles 5\nbatch-process-freq 1\nmin-ins 1000\nmax-ins 2000\n"
                "delay-per-exec 0\nmax-overall-mem 1024\nmem-per-frame 64\nmin-mem-per-proc 128\nmax-mem-per-proc 512\n",
                config, error));
    CHECK(config.maxOverallMem == 1024);
    CHECK(config.memPerFrame == 64);
    CHECK(config.minMemPerProc == 128);
    CHECK(config.maxMemPerProc == 512);
}

TEST(memoryParametersDefaultWhenMissing) {
    Config config;
    config.maxOverallMem = 64;
    std::string error;
    CHECK(parse("num-cpu 2", config, error));
    CHECK(config.maxOverallMem == 16384);
    CHECK(config.memPerFrame == 256);
    CHECK(config.minMemPerProc == 4096);
    CHECK(config.maxMemPerProc == 4096);
}

TEST(acceptsMaxMemProcSpelling) {
    Config config;
    std::string error;
    CHECK(parse("min-mem-per-proc 64 max-mem-proc 128", config, error));
    CHECK(config.maxMemPerProc == 128);
}

TEST(parsesTypedMemorySizes) {
    CHECK(parseMemorySize("256") == std::optional<uint64_t>(256));
    CHECK(parseMemorySize("65536") == std::optional<uint64_t>(65536));
    CHECK(!parseMemorySize("100").has_value());
    CHECK(!parseMemorySize("131072").has_value());
    CHECK(!parseMemorySize("abc").has_value());
    CHECK(!parseMemorySize("").has_value());
    CHECK(!parseMemorySize("99999999999999999999999").has_value());
}

TEST(memorySizesArePowersOfTwoFrom64To65536) {
    CHECK(isValidMemorySize(64));
    CHECK(isValidMemorySize(1024));
    CHECK(isValidMemorySize(65536));
    CHECK(!isValidMemorySize(0));
    CHECK(!isValidMemorySize(32));
    CHECK(!isValidMemorySize(100));
    CHECK(!isValidMemorySize(131072));
}

TEST(rejectsMemoryValuesThatAreNotValidSizes) {
    Config config;
    std::string error;
    CHECK(!parse("max-overall-mem 1000", config, error));
    CHECK(contains(error, "max-overall-mem"));
    CHECK(contains(error, "must be a power of 2 from 64 to 65536"));
    CHECK(!parse("mem-per-frame 32", config, error));
    CHECK(!parse("min-mem-per-proc 131072", config, error));
    CHECK(!parse("max-mem-per-proc abc", config, error));
    CHECK(!parse("max-mem-per-proc 99999999999999999999999", config, error));
}

TEST(rejectsFrameLargerThanMemory) {
    Config config;
    std::string error;
    CHECK(!parse("max-overall-mem 256 mem-per-frame 512", config, error));
    CHECK(contains(error, "mem-per-frame"));
    CHECK(parse("max-overall-mem 256 mem-per-frame 256", config, error));
}

TEST(rejectsMinMemAboveMaxMem) {
    Config config;
    std::string error;
    CHECK(!parse("min-mem-per-proc 2048 max-mem-per-proc 1024", config, error));
    CHECK(contains(error, "min-mem-per-proc"));
}

TEST(leavesConfigUnchangedOnMemoryError) {
    Config config;
    config.maxOverallMem = 512;
    std::string error;
    CHECK(!parse("max-overall-mem 1024 mem-per-frame 100", config, error));
    CHECK(config.maxOverallMem == 512);
}

// ---- Instruction ----

namespace {

int loopDepth(const Instruction& statement) {
    if (statement.type != InstructionType::FOR) {
        return 0;
    }
    int deepest = 0;
    for (const Instruction& child : statement.body) {
        deepest = std::max(deepest, loopDepth(child));
    }
    return 1 + deepest;
}

bool loopsAreWellFormed(const Instruction& statement) {
    if (statement.type != InstructionType::FOR) {
        return true;
    }
    if (statement.repeats < 2 || statement.body.empty()) {
        return false;
    }
    for (const Instruction& child : statement.body) {
        if (!loopsAreWellFormed(child)) {
            return false;
        }
    }
    return true;
}

bool isPoolVariable(const std::string& name) {
    return name == "x" || name == "y" || name == "z";
}

Instruction makeInstruction(InstructionType type) {
    Instruction instruction;
    instruction.type = type;
    return instruction;
}

Instruction makeLoop(uint16_t repeats, std::vector<Instruction> body) {
    Instruction loop = makeInstruction(InstructionType::FOR);
    loop.repeats = repeats;
    loop.body = std::move(body);
    return loop;
}

}  // namespace

TEST(executedCountMultipliesNestedLoops) {
    Instruction print = makeInstruction(InstructionType::PRINT);
    Instruction inner = makeLoop(2, {print, print});
    Instruction outer = makeLoop(3, {print, inner});
    CHECK(executedCount(print) == 1);
    CHECK(executedCount(inner) == 4);
    CHECK(executedCount(outer) == 15);
}

TEST(singleInstructionBudgetNeverMakesALoop) {
    bool noLoop = true;
    for (uint32_t seed = 1; seed <= 500; ++seed) {
        std::minstd_rand rng(seed);
        Instruction statement = randomStatement(rng, "p01", 1024, 0, 1);
        noLoop = noLoop && statement.type != InstructionType::FOR && executedCount(statement) == 1;
    }
    CHECK(noLoop);
}

TEST(randomStatementsStayWithinTheirBudget) {
    const uint64_t budgets[] = {2, 3, 7, 50, 1000, 4294967296ULL};
    bool withinBudget = true;
    bool wellFormed = true;
    bool shallowEnough = true;
    for (uint64_t budget : budgets) {
        for (uint32_t seed = 1; seed <= 2000; ++seed) {
            std::minstd_rand rng(seed);
            Instruction statement = randomStatement(rng, "p01", 1024, 0, budget);
            uint64_t count = executedCount(statement);
            withinBudget = withinBudget && count >= 1 && count <= budget;
            wellFormed = wellFormed && loopsAreWellFormed(statement);
            shallowEnough = shallowEnough && loopDepth(statement) <= kMaxForNesting;
        }
    }
    CHECK(withinBudget);
    CHECK(wellFormed);
    CHECK(shallowEnough);
}

TEST(generatesEveryInstructionType) {
    std::set<InstructionType> seen;
    for (uint32_t seed = 1; seed <= 2000; ++seed) {
        std::minstd_rand rng(seed);
        seen.insert(randomStatement(rng, "p01", 1024, 0, 1000).type);
    }
    CHECK(seen.size() == 8);
    CHECK(seen.count(InstructionType::READ) == 1);
    CHECK(seen.count(InstructionType::WRITE) == 1);
}

TEST(readAndWriteUseEvenAddressesPastTheSymbolTable) {
    bool foundRead = false;
    bool foundWrite = false;
    bool valid = true;
    for (uint32_t seed = 1; seed <= 2000; ++seed) {
        std::minstd_rand rng(seed);
        Instruction statement = randomStatement(rng, "p01", 1024, 0, 1);
        if (statement.type == InstructionType::READ) {
            foundRead = true;
            valid = valid && isPoolVariable(statement.variable);
        }
        if (statement.type == InstructionType::WRITE) {
            foundWrite = true;
            valid = valid && (!statement.first.isVariable || isPoolVariable(statement.first.variable));
        }
        if (statement.type == InstructionType::READ || statement.type == InstructionType::WRITE) {
            valid = valid && statement.address % 2 == 0 && statement.address >= 64 && statement.address <= 1022;
        }
    }
    CHECK(foundRead);
    CHECK(foundWrite);
    CHECK(valid);
}

TEST(smallestProcessesReadAndWriteAnywhereInTheirMemory) {
    bool found = false;
    bool inRange = true;
    for (uint32_t seed = 1; seed <= 2000; ++seed) {
        std::minstd_rand rng(seed);
        Instruction statement = randomStatement(rng, "p01", 64, 0, 1);
        if (statement.type == InstructionType::READ || statement.type == InstructionType::WRITE) {
            found = true;
            inRange = inRange && statement.address % 2 == 0 && statement.address <= 62;
        }
    }
    CHECK(found);
    CHECK(inRange);
}

TEST(neverStartsALoopAtTheNestingLimit) {
    bool noLoop = true;
    std::minstd_rand rng(7);
    for (int i = 0; i < 1000; ++i) {
        noLoop = noLoop && randomStatement(rng, "p01", 1024, kMaxForNesting, 1000).type != InstructionType::FOR;
    }
    CHECK(noLoop);
}

TEST(printUsesTheDefaultHelloWorldMessage) {
    bool foundPrint = false;
    bool defaultMessage = true;
    std::minstd_rand rng(3);
    for (int i = 0; i < 300; ++i) {
        Instruction statement = randomStatement(rng, "screen_01", 1024, 0, 1);
        if (statement.type == InstructionType::PRINT) {
            foundPrint = true;
            defaultMessage = defaultMessage && statement.message == "Hello world from screen_01!" &&
                             statement.variable.empty();
        }
    }
    CHECK(foundPrint);
    CHECK(defaultMessage);
}

TEST(sleepLastsOneToTenTicks) {
    bool foundSleep = false;
    bool inRange = true;
    std::minstd_rand rng(5);
    for (int i = 0; i < 300; ++i) {
        Instruction statement = randomStatement(rng, "p01", 1024, 0, 1);
        if (statement.type == InstructionType::SLEEP) {
            foundSleep = true;
            inRange = inRange && statement.sleepTicks >= 1 && statement.sleepTicks <= 10;
        }
    }
    CHECK(foundSleep);
    CHECK(inRange);
}

TEST(arithmeticUsesVariablesFromThePool) {
    bool foundArithmetic = false;
    bool valid = true;
    std::minstd_rand rng(11);
    for (int i = 0; i < 300; ++i) {
        Instruction statement = randomStatement(rng, "p01", 1024, 0, 1);
        if (statement.type == InstructionType::DECLARE) {
            valid = valid && isPoolVariable(statement.variable);
        }
        if (statement.type == InstructionType::ADD || statement.type == InstructionType::SUBTRACT) {
            foundArithmetic = true;
            valid = valid && isPoolVariable(statement.variable) &&
                    (!statement.first.isVariable || isPoolVariable(statement.first.variable)) &&
                    (!statement.second.isVariable || isPoolVariable(statement.second.variable));
        }
    }
    CHECK(foundArithmetic);
    CHECK(valid);
}

// ---- Instruction parser ----

namespace {

// The spec's sample screen -c program, as it reads once the \" escapes are removed.
const char* const kSpecProgram =
    "DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x500 varA; READ varC 0x500; "
    "PRINT(\"Result: \" + varC)";

// count copies of statement, separated by semicolons.
std::string repeated(const std::string& statement, int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += (i > 0 ? "; " : "") + statement;
    }
    return text;
}

bool isVariable(const Operand& operand, const std::string& name) {
    return operand.isVariable && operand.variable == name;
}

bool isLiteral(const Operand& operand, uint16_t value) {
    return !operand.isVariable && operand.value == value;
}

}  // namespace

TEST(parsesTheSpecSampleProgram) {
    std::optional<std::vector<Instruction>> program = parseProgram(kSpecProgram);
    CHECK(program.has_value() && program->size() == 6);
    if (!program || program->size() != 6) {
        return;
    }
    const std::vector<Instruction>& p = *program;
    CHECK(p[0].type == InstructionType::DECLARE && p[0].variable == "varA" && p[0].first.value == 10);
    CHECK(p[1].type == InstructionType::DECLARE && p[1].variable == "varB" && p[1].first.value == 5);
    CHECK(p[2].type == InstructionType::ADD && p[2].variable == "varA" && isVariable(p[2].first, "varA") &&
          isVariable(p[2].second, "varB"));
    CHECK(p[3].type == InstructionType::WRITE && p[3].address == 0x500 && isVariable(p[3].first, "varA"));
    CHECK(p[4].type == InstructionType::READ && p[4].variable == "varC" && p[4].address == 1280);
    CHECK(p[5].type == InstructionType::PRINT && p[5].message == "Result: " && p[5].variable == "varC");
}

TEST(parsesTheParenthesizedForm) {
    std::optional<std::vector<Instruction>> program =
        parseProgram("DECLARE(varA, 10); ADD(varA, varA, 5); SUBTRACT(x, 7, y); SLEEP(3); READ(v, 0x40); WRITE(0x42, 9)");
    CHECK(program.has_value() && program->size() == 6);
    if (!program || program->size() != 6) {
        return;
    }
    const std::vector<Instruction>& p = *program;
    CHECK(p[0].type == InstructionType::DECLARE && p[0].variable == "varA" && p[0].first.value == 10);
    CHECK(p[1].type == InstructionType::ADD && p[1].variable == "varA" && isVariable(p[1].first, "varA") &&
          isLiteral(p[1].second, 5));
    CHECK(p[2].type == InstructionType::SUBTRACT && p[2].variable == "x" && isLiteral(p[2].first, 7) &&
          isVariable(p[2].second, "y"));
    CHECK(p[3].type == InstructionType::SLEEP && p[3].sleepTicks == 3);
    CHECK(p[4].type == InstructionType::READ && p[4].variable == "v" && p[4].address == 0x40);
    CHECK(p[5].type == InstructionType::WRITE && p[5].address == 0x42 && isLiteral(p[5].first, 9));
}

TEST(addressesAreHexWithOrWithoutPrefix) {
    std::optional<std::vector<Instruction>> program = parseProgram("WRITE 42 9; READ v 0X1f; READ w 0x00A0");
    CHECK(program.has_value() && program->size() == 3);
    if (program && program->size() == 3) {
        CHECK((*program)[0].address == 0x42);
        CHECK((*program)[1].address == 0x1F);
        CHECK((*program)[2].address == 0xA0);
    }
}

TEST(printTakesAMessageAVariableOrBoth) {
    std::optional<std::vector<Instruction>> program = parseProgram("PRINT(\"hi\"); PRINT(x); PRINT(\"x is \"+x)");
    CHECK(program.has_value() && program->size() == 3);
    if (program && program->size() == 3) {
        const std::vector<Instruction>& p = *program;
        CHECK(p[0].message == "hi" && p[0].variable.empty());
        CHECK(p[1].message.empty() && p[1].variable == "x");
        CHECK(p[2].message == "x is " && p[2].variable == "x");
    }
}

TEST(parsesForLoops) {
    std::optional<std::vector<Instruction>> program = parseProgram("FOR([PRINT(\"hi\"); ADD x x 1], 3)");
    CHECK(program.has_value() && program->size() == 1);
    if (!program || program->size() != 1) {
        return;
    }
    const Instruction& loop = (*program)[0];
    CHECK(loop.type == InstructionType::FOR && loop.repeats == 3 && loop.body.size() == 2);
    if (loop.body.size() == 2) {
        CHECK(loop.body[0].type == InstructionType::PRINT && loop.body[0].message == "hi");
        CHECK(loop.body[1].type == InstructionType::ADD && isVariable(loop.body[1].first, "x") &&
              isLiteral(loop.body[1].second, 1));
    }
    CHECK(executedCount(loop) == 6);
}

TEST(forLoopsNestAtMostThreeDeep) {
    CHECK(parseProgram("FOR([FOR([FOR([PRINT(\"deep\")], 2)], 2)], 2)").has_value());
    CHECK(!parseProgram("FOR([FOR([FOR([FOR([PRINT(\"deep\")], 2)], 2)], 2)], 2)").has_value());
}

TEST(rejectsLoopsThatRunNothing) {
    CHECK(!parseProgram("FOR([], 3)").has_value());
    CHECK(!parseProgram("FOR([PRINT(\"hi\")], 0)").has_value());
}

TEST(programsHaveOneToFiftyStatements) {
    CHECK(!parseProgram("").has_value());
    CHECK(!parseProgram("  ;  ").has_value());
    CHECK(parseProgram(repeated("SLEEP 1", 1)).has_value());
    CHECK(parseProgram(repeated("SLEEP 1", 50)).has_value());
    CHECK(!parseProgram(repeated("SLEEP 1", 51)).has_value());
}

TEST(literalsClampToUint16) {
    std::optional<std::vector<Instruction>> program =
        parseProgram("DECLARE x 70000; DECLARE y -5; ADD z 99999999999999999999 1; SLEEP 300");
    CHECK(program.has_value() && program->size() == 4);
    if (program && program->size() == 4) {
        const std::vector<Instruction>& p = *program;
        CHECK(p[0].first.value == 65535);
        CHECK(p[1].first.value == 0);
        CHECK(isLiteral(p[2].first, 65535));
        CHECK(p[3].sleepTicks == 255);
    }
}

TEST(rejectsInvalidStatements) {
    CHECK(!parseProgram("FOO 1").has_value());
    CHECK(!parseProgram("READ x zz").has_value());
    CHECK(!parseProgram("PRINT(unterminated").has_value());
    CHECK(!parseProgram("PRINT(\"unterminated)").has_value());
    CHECK(!parseProgram("DECLARE x").has_value());
    CHECK(!parseProgram("DECLARE 5 x").has_value());
    CHECK(!parseProgram("ADD x y").has_value());
    CHECK(!parseProgram("WRITE 0x10 1 2").has_value());
    CHECK(!parseProgram("SLEEP 1; FOO 2").has_value());
}

TEST(ignoresATrailingSemicolon) {
    std::optional<std::vector<Instruction>> program = parseProgram("DECLARE x 1; PRINT(\"done\");");
    CHECK(program.has_value() && program->size() == 2);
}

TEST(semicolonsInsideQuotesDoNotSplit) {
    std::optional<std::vector<Instruction>> program = parseProgram("PRINT(\"a; b\"); SLEEP 1");
    CHECK(program.has_value() && program->size() == 2);
    if (program && program->size() == 2) {
        CHECK((*program)[0].message == "a; b");
    }
}

TEST(keywordsIgnoreCase) {
    std::optional<std::vector<Instruction>> program =
        parseProgram("declare x 1; Print(\"v\" + x); write 0x40 x; Read y 0x40; for([sleep 1], 2)");
    CHECK(program.has_value() && program->size() == 5);
    if (program && program->size() == 5) {
        const std::vector<Instruction>& p = *program;
        CHECK(p[0].type == InstructionType::DECLARE);
        CHECK(p[1].type == InstructionType::PRINT);
        CHECK(p[2].type == InstructionType::WRITE);
        CHECK(p[3].type == InstructionType::READ);
        CHECK(p[4].type == InstructionType::FOR);
    }
}

TEST(screenCreateArgumentsWithoutASize) {
    std::optional<ScreenCreateArgs> args = parseScreenCreate("process2 \"DECLARE x 1\"");
    CHECK(args.has_value());
    if (args) {
        CHECK(args->name == "process2");
        CHECK(args->sizeText.empty());
        CHECK(args->instructions == "DECLARE x 1");
    }
}

TEST(screenCreateArgumentsWithASize) {
    std::optional<ScreenCreateArgs> args = parseScreenCreate("p 256 \"SLEEP 1\"");
    CHECK(args.has_value() && args->name == "p" && args->sizeText == "256" && args->instructions == "SLEEP 1");
    std::optional<ScreenCreateArgs> empty = parseScreenCreate("q 256 \"\"");
    CHECK(empty.has_value() && empty->instructions.empty());
}

TEST(screenCreateUnescapesQuotes) {
    std::optional<ScreenCreateArgs> args =
        parseScreenCreate("process2 \"DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x500 varA; "
                          "READ varC 0x500; PRINT(\\\"Result: \\\" + varC)\"");
    CHECK(args.has_value() && args->instructions == kSpecProgram);
}

TEST(screenCreateNeedsQuotedInstructions) {
    CHECK(!parseScreenCreate("").has_value());
    CHECK(!parseScreenCreate("p").has_value());
    CHECK(!parseScreenCreate("p 256").has_value());
    CHECK(!parseScreenCreate("p 256 SLEEP 1").has_value());
    CHECK(!parseScreenCreate("p \"SLEEP 1\" extra").has_value());
}

// ---- Backing store ----

namespace {

// A path in the temp folder, so tests don't leave files in the repo.
std::string tempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

std::vector<std::string> fileLines(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    return lines;
}

const std::string kStorePath = tempPath("csopesy-test-backing-store.txt");

}  // namespace

TEST(backingStoreCreatesItsFile) {
    std::remove(kStorePath.c_str());
    BackingStore store(kStorePath);
    std::vector<std::string> lines = fileLines(kStorePath);
    CHECK(lines.size() == 1 && lines[0] == kBackingStoreHeader);
}

TEST(backingStoreWritesStoredPagesOnFlush) {
    BackingStore store(kStorePath);
    std::vector<uint8_t> bytes(64, 0);
    bytes[0] = 10;
    store.store(1, "p01", 2, bytes);
    CHECK(fileLines(kStorePath).size() == 1); // written only by flush
    store.flush();
    std::string expected = "p01 (ID 1) page 2: 0a";
    for (int i = 1; i < 64; ++i) {
        expected += " 00";
    }
    std::vector<std::string> lines = fileLines(kStorePath);
    CHECK(lines.size() == 2 && lines[1] == expected);
    CHECK(store.pageCount() == 1);
}

TEST(backingStoreTakeReturnsAndRemovesThePage) {
    BackingStore store(kStorePath);
    store.store(1, "p01", 2, {1, 2, 255});
    store.flush();
    std::optional<std::vector<uint8_t>> page = store.take(1, 2);
    CHECK(page.has_value() && *page == std::vector<uint8_t>({1, 2, 255}));
    CHECK(!store.take(1, 2).has_value());
    CHECK(!store.take(9, 0).has_value());
    store.flush();
    CHECK(fileLines(kStorePath).size() == 1);
    CHECK(store.pageCount() == 0);
}

TEST(backingStoreRemovesAllPagesOfAProcess) {
    BackingStore store(kStorePath);
    store.store(1, "a", 0, {1});
    store.store(1, "a", 5, {2});
    store.store(2, "b", 0, {3});
    store.removeProcess(1);
    store.flush();
    std::vector<std::string> lines = fileLines(kStorePath);
    CHECK(lines.size() == 2 && lines[1] == "b (ID 2) page 0: 03");
    CHECK(store.pageCount() == 1);
}

TEST(backingStoreFlushSkipsTheFileWhenNothingChanged) {
    BackingStore store(kStorePath);
    store.store(1, "a", 0, {1});
    store.flush();
    {
        std::ofstream file(kStorePath, std::ios::app);
        file << "marker\n";
    }
    store.flush(); // nothing changed since the last flush, so the marker stays
    std::vector<std::string> lines = fileLines(kStorePath);
    CHECK(!lines.empty() && lines.back() == "marker");
    std::remove(kStorePath.c_str());
}

// ---- Memory manager ----
// Most tests use 256 bytes of memory in 64-byte frames: 4 frames.

TEST(freshPagesReadZero) {
    MemoryManager memory(256, 64, kStorePath);
    memory.allocate(1, "p", 256);
    CHECK(memory.isAllocated(1) && !memory.isAllocated(2));
    CHECK(!memory.isResident(1, 0));
    CHECK(memory.pageIn(1, {0}));
    CHECK(memory.isResident(1, 0));
    CHECK(memory.read16(1, 10) == 0);
    CHECK(memory.pagedIn() == 1 && memory.pagedOut() == 0);
}

TEST(write16RoundTripsLittleEndianAcrossPages) {
    MemoryManager memory(256, 64, kStorePath);
    memory.allocate(1, "p", 256);
    CHECK(memory.pageIn(1, {0, 1}));
    memory.write16(1, 10, 0x1234);
    CHECK(memory.read16(1, 10) == 0x1234);
    memory.write16(1, 63, 0xABCD); // low byte at 63 (page 0), high byte at 64 (page 1)
    CHECK(memory.read16(1, 63) == 0xABCD);
    CHECK(memory.read16(1, 64) == 0x00AB);
    CHECK(memory.read16(1, 62) == 0xCD00);
}

TEST(fullMemoryEvictsTheOldestPageToTheBackingStore) {
    MemoryManager memory(256, 64, kStorePath);
    memory.allocate(1, "p01", 512);
    for (uint64_t page = 0; page < 4; ++page) {
        CHECK(memory.pageIn(1, {page}));
        memory.unpin(1);
    }
    memory.write16(1, 0, 42);
    CHECK(memory.pageIn(1, {4}));
    CHECK(!memory.isResident(1, 0));
    CHECK(memory.isResident(1, 1) && memory.isResident(1, 4));
    CHECK(memory.pagedOut() == 1);
    memory.flush();
    std::vector<std::string> lines = fileLines(kStorePath);
    CHECK(lines.size() == 2 && lines[1].rfind("p01 (ID 1) page 0: 2a 00 ", 0) == 0);

    memory.unpin(1);
    CHECK(memory.pageIn(1, {0})); // evicts page 1, now the oldest
    CHECK(memory.read16(1, 0) == 42);
    CHECK(!memory.isResident(1, 1));
    CHECK(memory.pagedIn() == 6 && memory.pagedOut() == 2);
}

TEST(pinnedPagesAreNeverEvicted) {
    MemoryManager memory(256, 64, kStorePath);
    memory.allocate(1, "a", 256);
    memory.allocate(2, "b", 512);
    CHECK(memory.pageIn(1, {0, 1}));
    CHECK(memory.pageIn(2, {0, 1}));
    CHECK(!memory.pageIn(2, {2})); // every frame is pinned
    CHECK(memory.residentBytes(2) == 128 && !memory.isResident(2, 2));
    memory.unpin(1);
    CHECK(memory.pageIn(2, {2})); // evicts a's oldest page
    CHECK(!memory.isResident(1, 0) && memory.isResident(1, 1));
}

TEST(pageInIsAllOrNothing) {
    MemoryManager memory(256, 64, kStorePath);
    memory.allocate(1, "a", 256);
    memory.allocate(2, "b", 512);
    CHECK(memory.pageIn(1, {0, 1, 2}));
    CHECK(!memory.pageIn(2, {0, 1})); // needs two frames; only one is free
    CHECK(memory.residentBytes(2) == 0 && memory.pagedIn() == 3);
    CHECK(memory.pageIn(2, {0}));
}

TEST(pageInKeepsTheInstructionsOtherPages) {
    MemoryManager memory(128, 64, kStorePath); // 2 frames
    memory.allocate(1, "a", 256);
    CHECK(memory.pageIn(1, {0}));
    memory.unpin(1);
    CHECK(memory.pageIn(1, {1}));
    memory.unpin(1);
    CHECK(memory.pageIn(1, {0, 2})); // evicts page 1, not page 0, the oldest
    CHECK(memory.isResident(1, 0) && memory.isResident(1, 2) && !memory.isResident(1, 1));
}

TEST(usedMemoryFollowsLoadedPages) {
    MemoryManager memory(256, 64, kStorePath);
    CHECK(memory.totalBytes() == 256 && memory.frameBytes() == 64 && memory.usedBytes() == 0);
    memory.allocate(1, "a", 256);
    memory.allocate(2, "b", 64);
    CHECK(memory.usedBytes() == 0);
    CHECK(memory.pageIn(1, {0, 3}));
    CHECK(memory.pageIn(2, {0}));
    CHECK(memory.usedBytes() == 192);
    CHECK(memory.residentBytes(1) == 128 && memory.residentBytes(2) == 64 && memory.residentBytes(3) == 0);
}

TEST(releaseFreesFramesAndSavedPages) {
    MemoryManager memory(128, 64, kStorePath);
    memory.allocate(1, "a", 256);
    CHECK(memory.pageIn(1, {0, 1}));
    memory.unpin(1);
    CHECK(memory.pageIn(1, {2})); // evicts page 0
    memory.flush();
    CHECK(fileLines(kStorePath).size() == 2);
    memory.release(1);
    memory.flush();
    CHECK(!memory.isAllocated(1));
    CHECK(memory.usedBytes() == 0);
    CHECK(fileLines(kStorePath).size() == 1);
    memory.allocate(2, "b", 128);
    CHECK(memory.pageIn(2, {0, 1})); // both frames are free again
    CHECK(memory.pagedOut() == 1);
    std::remove(kStorePath.c_str());
}

// ---- Process ----

namespace {

Instruction makePrint(const std::string& message, const std::string& variable = "") {
    Instruction print = makeInstruction(InstructionType::PRINT);
    print.message = message;
    print.variable = variable;
    return print;
}

Instruction makeDeclare(const std::string& variable, uint16_t value) {
    Instruction declare = makeInstruction(InstructionType::DECLARE);
    declare.variable = variable;
    declare.first.value = value;
    return declare;
}

Operand literal(uint16_t value) {
    Operand operand;
    operand.value = value;
    return operand;
}

Operand variableOperand(const std::string& name) {
    Operand operand;
    operand.isVariable = true;
    operand.variable = name;
    return operand;
}

Instruction makeArithmetic(InstructionType type, const std::string& target, Operand first, Operand second) {
    Instruction arithmetic = makeInstruction(type);
    arithmetic.variable = target;
    arithmetic.first = first;
    arithmetic.second = second;
    return arithmetic;
}

Instruction makeSleep(uint8_t ticks) {
    Instruction sleep = makeInstruction(InstructionType::SLEEP);
    sleep.sleepTicks = ticks;
    return sleep;
}

Instruction makeRead(const std::string& variable, uint64_t address) {
    Instruction read = makeInstruction(InstructionType::READ);
    read.variable = variable;
    read.address = address;
    return read;
}

Instruction makeWrite(uint64_t address, Operand value) {
    Instruction write = makeInstruction(InstructionType::WRITE);
    write.address = address;
    write.first = value;
    return write;
}

const std::string kProcessStore = tempPath("csopesy-test-process-store.txt");

// Pages in what the process's next instruction needs (allocating its memory first), then runs it.
StepResult runOne(Process& process, MemoryManager& memory, int core = 0) {
    memory.allocate(process.getId(), process.getName(), process.getMemoryBytes());
    memory.pageIn(process.getId(), process.pagesFor(process.peek(), memory.frameBytes()));
    StepResult result = process.step(core, memory);
    memory.unpin(process.getId());
    return result;
}

// Runs the process until it finishes, at most limit steps, and returns the number of steps.
int runToEnd(Process& process, MemoryManager& memory, int limit = 1000) {
    int steps = 0;
    while (!process.isFinished() && steps < limit) {
        runOne(process, memory);
        ++steps;
    }
    return steps;
}

// A 1024-byte process with its symbol table page loaded, for calling execute() directly.
struct LoadedProcess {
    MemoryManager memory{16384, 256, kProcessStore};
    Process process{1, "p", 1024, 10, 1};

    LoadedProcess() {
        memory.allocate(1, "p", 1024);
        memory.pageIn(1, {0});
    }

    bool run(const Instruction& instruction, int core = 0) {
        return process.execute(instruction, core, memory);
    }

    bool has(const std::string& variable, uint16_t expected) const {
        std::optional<uint16_t> value = process.variable(variable, memory);
        return value.has_value() && *value == expected;
    }
};

}  // namespace

TEST(newProcessStartsAtLineZero) {
    std::time_t before = std::time(nullptr);
    Process process(7, "screen_01", 1024, 50, 1);
    std::time_t after = std::time(nullptr);
    CHECK(process.getId() == 7);
    CHECK(process.getName() == "screen_01");
    CHECK(process.getMemoryBytes() == 1024);
    CHECK(process.getTotalInstructions() == 50);
    CHECK(process.getExecuted() == 0);
    CHECK(!process.isFinished() && !process.hasCrashed());
    CHECK(process.variableCount() == 0);
    CHECK(process.getCreatedAt() >= before && process.getCreatedAt() <= after);
}

TEST(declareSetsTheVariable) {
    LoadedProcess loaded;
    CHECK(loaded.run(makeDeclare("x", 5)));
    CHECK(loaded.has("x", 5));
}

TEST(addStoresTheSum) {
    LoadedProcess loaded;
    loaded.run(makeArithmetic(InstructionType::ADD, "x", literal(3), literal(4)));
    CHECK(loaded.has("x", 7));
}

TEST(addClampsAtUint16Max) {
    LoadedProcess loaded;
    loaded.run(makeArithmetic(InstructionType::ADD, "x", literal(65000), literal(1000)));
    CHECK(loaded.has("x", 65535));
}

TEST(subtractClampsAtZero) {
    LoadedProcess loaded;
    loaded.run(makeArithmetic(InstructionType::SUBTRACT, "x", literal(5), literal(10)));
    CHECK(loaded.has("x", 0));
}

TEST(arithmeticReadsVariables) {
    LoadedProcess loaded;
    loaded.run(makeDeclare("y", 10));
    loaded.run(makeArithmetic(InstructionType::ADD, "x", variableOperand("y"), variableOperand("y")));
    loaded.run(makeArithmetic(InstructionType::SUBTRACT, "z", variableOperand("x"), literal(5)));
    CHECK(loaded.has("x", 20));
    CHECK(loaded.has("z", 15));
}

TEST(undeclaredVariablesAreDeclaredAsZero) {
    LoadedProcess loaded;
    loaded.run(makeArithmetic(InstructionType::ADD, "x", variableOperand("y"), literal(1)));
    CHECK(loaded.has("x", 1));
    CHECK(loaded.has("y", 0));
}

TEST(variablesAreStoredInTheSymbolTable) {
    LoadedProcess loaded;
    loaded.run(makeDeclare("a", 5));
    loaded.run(makeDeclare("b", 0x1234));
    CHECK(loaded.memory.read16(1, 0) == 5);      // variable 0 is at address 0
    CHECK(loaded.memory.read16(1, 2) == 0x1234); // variable 1 is at address 2
    CHECK(loaded.process.variableCount() == 2);
}

TEST(theSymbolTableHoldsThirtyTwoVariables) {
    LoadedProcess loaded;
    bool declared = true;
    for (int i = 0; i < 32; ++i) {
        declared = declared && loaded.run(makeDeclare("v" + std::to_string(i), static_cast<uint16_t>(i + 1)));
    }
    CHECK(declared);
    CHECK(loaded.run(makeDeclare("v32", 99))); // ignored: the symbol table is full
    CHECK(loaded.process.variableCount() == 32);
    CHECK(!loaded.process.variable("v32", loaded.memory).has_value());
    CHECK(loaded.has("v0", 1) && loaded.has("v31", 32));
    // Any instruction that would declare a 33rd variable is ignored as a whole.
    CHECK(loaded.run(makeArithmetic(InstructionType::ADD, "v31", variableOperand("v32"), literal(1))));
    CHECK(loaded.has("v31", 32));
    CHECK(loaded.run(makePrint("v32 = ", "v32")));
    CHECK(loaded.process.getLogs().empty());
    // Instructions that only use declared variables still run.
    CHECK(loaded.run(makeArithmetic(InstructionType::ADD, "v31", variableOperand("v31"), literal(1))));
    CHECK(loaded.has("v31", 33));
}

TEST(ignoredDeclarationsStillCountAsExecuted) {
    std::vector<Instruction> program;
    for (int i = 0; i < 33; ++i) {
        program.push_back(makeDeclare("v" + std::to_string(i), 7));
    }
    program.push_back(makePrint("done"));
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "p", 1024, program);
    for (int i = 0; i < 33; ++i) {
        runOne(process, memory);
    }
    CHECK(process.getExecuted() == 33);
    CHECK(process.variableCount() == 32);
    CHECK(!process.variable("v32", memory).has_value());
}

TEST(printLogsTheMessageWithCoreAndTime) {
    LoadedProcess loaded;
    std::time_t before = std::time(nullptr);
    loaded.run(makePrint("Hello world from p01!"), 2);
    std::time_t after = std::time(nullptr);
    const std::vector<LogEntry>& logs = loaded.process.getLogs();
    CHECK(logs.size() == 1);
    if (logs.size() == 1) {
        CHECK(logs[0].text == "Hello world from p01!");
        CHECK(logs[0].core == 2);
        CHECK(logs[0].time >= before && logs[0].time <= after);
    }
}

TEST(printAppendsTheVariableValue) {
    LoadedProcess loaded;
    loaded.run(makeDeclare("x", 5));
    loaded.run(makePrint("Value from: ", "x"));
    loaded.run(makePrint("Undeclared: ", "w"));
    const std::vector<LogEntry>& logs = loaded.process.getLogs();
    CHECK(logs.size() == 2);
    if (logs.size() == 2) {
        CHECK(logs[0].text == "Value from: 5");
        CHECK(logs[1].text == "Undeclared: 0");
    }
}

TEST(writeThenReadGoesThroughMemory) {
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "p", 4096,
                    {makeDeclare("a", 42), makeWrite(0x500, variableOperand("a")), makeRead("b", 0x500),
                     makeRead("c", 0x200), makePrint("b = ", "b"), makePrint("c = ", "c")});
    CHECK(runToEnd(process, memory) == 6);
    const std::vector<LogEntry>& logs = process.getLogs();
    CHECK(logs.size() == 2);
    if (logs.size() == 2) {
        CHECK(logs[0].text == "b = 42");
        CHECK(logs[1].text == "c = 0"); // memory that was never written reads as 0
    }
}

TEST(specProgramPrintsResult15) {
    std::optional<std::vector<Instruction>> program = parseProgram(kSpecProgram);
    CHECK(program.has_value());
    if (!program) {
        return;
    }
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "process2", 4096, *program);
    CHECK(process.getTotalInstructions() == 6);
    for (int i = 0; i < 5; ++i) {
        runOne(process, memory);
    }
    CHECK(process.variable("varC", memory) == std::optional<uint16_t>(15));
    CHECK(memory.read16(1, 0x500) == 15);
    CHECK(runOne(process, memory) == StepResult::FINISHED);
    CHECK(!process.getLogs().empty() && process.getLogs().back().text == "Result: 15");
}

TEST(pagesForListsTheSymbolTableAndAddressPages) {
    using Pages = std::vector<uint64_t>;
    Process process(1, "p", 4096, 10, 1);
    auto pagesOf = [&process](const Instruction& instruction) { return process.pagesFor(instruction, 256); };
    CHECK(pagesOf(makeDeclare("x", 1)) == Pages({0}));
    CHECK(pagesOf(makeWrite(0x500, variableOperand("a"))) == Pages({0, 5}));
    CHECK(pagesOf(makeWrite(0x500, literal(3))) == Pages({5}));
    CHECK(pagesOf(makeRead("x", 0x1FF)) == Pages({0, 1, 2})); // the uint16 spans two pages
    CHECK(pagesOf(makePrint("hi")) == Pages());
    CHECK(pagesOf(makePrint("x = ", "x")) == Pages({0}));
    CHECK(pagesOf(makeSleep(2)) == Pages());
    CHECK(pagesOf(makeRead("x", 4095)) == Pages({0})); // an invalid address loads no page
}

TEST(accessOutsideTheProcessMemoryCrashesIt) {
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "f", 1024, {makeWrite(1022, literal(1)), makeWrite(1023, literal(2)), makePrint("never")});
    CHECK(runOne(process, memory) == StepResult::RAN); // the last whole uint16 in memory
    std::time_t before = std::time(nullptr);
    CHECK(runOne(process, memory) == StepResult::CRASHED);
    std::time_t after = std::time(nullptr);
    CHECK(process.hasCrashed() && process.isFinished());
    CHECK(process.faultAddress() == 1023);
    CHECK(process.crashedAt() >= before && process.crashedAt() <= after);
    CHECK(process.getExecuted() == 1 && process.getTotalInstructions() == 3);
    CHECK(process.getLogs().empty());
}

TEST(readOutsideTheProcessMemoryCrashesIt) {
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "f", 64, {makeRead("x", 0x500)});
    CHECK(runOne(process, memory) == StepResult::CRASHED);
    CHECK(process.faultAddress() == 0x500);
    CHECK(process.variableCount() == 0);
}

TEST(loopRepeatsItsBody) {
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "p", 1024, {makeLoop(3, {makePrint("hi")})});
    CHECK(process.getTotalInstructions() == 3);
    CHECK(runOne(process, memory) == StepResult::RAN);
    CHECK(runOne(process, memory) == StepResult::RAN);
    CHECK(runOne(process, memory) == StepResult::FINISHED);
    CHECK(process.getLogs().size() == 3);
    CHECK(process.getExecuted() == 3);
    CHECK(process.isFinished());
}

TEST(nestedLoopsRunEveryRepetition) {
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "p", 1024, {makeLoop(3, {makePrint("outer"), makeLoop(2, {makeDeclare("x", 1)})})});
    CHECK(runToEnd(process, memory, 20) == 9);
    CHECK(process.getLogs().size() == 3);
}

TEST(sleepReturnsSleptWithItsTicks) {
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "p", 1024, {makeLoop(2, {makeSleep(4)})});
    CHECK(runOne(process, memory) == StepResult::SLEPT);
    CHECK(process.getSleepTicks() == 4);
    CHECK(runOne(process, memory) == StepResult::FINISHED);
}

TEST(programLengthIsTheSumOfItsStatements) {
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "p", 1024, {makeDeclare("x", 1), makeLoop(3, {makePrint("a"), makePrint("b")}), makeSleep(1)});
    CHECK(process.getTotalInstructions() == 8);
    CHECK(runToEnd(process, memory) == 8);
}

TEST(randomProcessesRunExactlyTheirLength) {
    const uint64_t lengths[] = {1, 2, 3, 17, 1000};
    MemoryManager memory(16384, 256, kProcessStore);
    bool exact = true;
    bool released = true;
    bool noCrash = true;
    for (uint64_t length : lengths) {
        for (uint32_t seed = 1; seed <= 200; ++seed) {
            Process process(1, "p", 1024, length, seed);
            uint64_t steps = 0;
            while (!process.isFinished() && steps <= length) {
                noCrash = noCrash && runOne(process, memory, static_cast<int>(steps % 4)) != StepResult::CRASHED;
                ++steps;
            }
            exact = exact && steps == length && process.getExecuted() == length && process.isFinished();
            released = released && process.variableCount() == 0;
            memory.release(1);
        }
    }
    CHECK(exact);
    CHECK(released);
    CHECK(noCrash);
}

TEST(generatedProcessesRunTheRandomMix) {
    // Replaces MCO1's quiz pattern (PRINT and ADD on x, alternating): generated processes run the
    // random mix again, now with READ and WRITE, and start with no variables.
    MemoryManager memory(16384, 256, kProcessStore);
    Process process(1, "p", 1024, 2000, 3);
    CHECK(!process.variable("x", memory).has_value());
    std::set<InstructionType> seen;
    while (!process.isFinished()) {
        seen.insert(process.peek().type);
        runOne(process, memory);
    }
    CHECK(seen.size() == 7 && seen.count(InstructionType::FOR) == 0);
    CHECK(process.getExecuted() == 2000);
    std::remove(kProcessStore.c_str());
}

// ---- Scheduler ----

namespace {

Config testConfig(uint32_t cores, SchedulerType type, uint64_t quantum, uint64_t minIns, uint64_t maxIns,
                  uint64_t delay, uint64_t batchFrequency = 1) {
    Config config;
    config.numCpu = cores;
    config.scheduler = type;
    config.quantumCycles = quantum;
    config.minIns = minIns;
    config.maxIns = maxIns;
    config.delayPerExec = delay;
    config.batchProcessFreq = batchFrequency;
    return config;
}

const std::string kSchedulerStore = tempPath("csopesy-test-scheduler-store.txt");

std::string onFirstCore(const Utilization& usage) {
    return usage.running.empty() ? "" : usage.running[0].name;
}

uint64_t executedOnFirstCore(const Utilization& usage) {
    return usage.running.empty() ? 0 : usage.running[0].executed;
}

uint64_t executedOf(const Scheduler& scheduler, const std::string& name) {
    std::optional<ProcessView> view = scheduler.view(name);
    return view ? view->executed : 0;
}

bool hasFinished(const Scheduler& scheduler, const std::string& name) {
    std::optional<ProcessView> view = scheduler.view(name);
    return view && view->finished;
}

// Ticks until the named process finishes, at most limit times.
void tickUntilFinished(Scheduler& scheduler, const std::string& name, int limit) {
    for (int i = 0; i < limit && !hasFinished(scheduler, name); ++i) {
        scheduler.tick();
    }
}

}  // namespace

TEST(createsProcessesWithUniqueNames) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 10, 10, 0), 1, kSchedulerStore);
    CHECK(scheduler.createProcess("a", 1024));
    CHECK(!scheduler.createProcess("a", 1024));
    std::optional<ProcessView> view = scheduler.view("a");
    CHECK(view.has_value());
    if (view) {
        CHECK(view->name == "a");
        CHECK(view->id == 1);
        CHECK(view->memoryBytes == 1024);
        CHECK(view->total == 10);
        CHECK(view->executed == 0);
        CHECK(!view->finished && !view->crashed);
    }
    CHECK(!scheduler.view("missing").has_value());
}

TEST(processLengthsFollowMinAndMaxIns) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 3, 7, 0), 1, kSchedulerStore);
    bool inRange = true;
    for (int i = 0; i < 100; ++i) {
        std::string name = "proc" + std::to_string(i);
        scheduler.createProcess(name, 1024);
        std::optional<ProcessView> view = scheduler.view(name);
        inRange = inRange && view && view->total >= 3 && view->total <= 7;
    }
    CHECK(inRange);
}

TEST(idleCoresTakeReadyProcessesInOrder) {
    Scheduler scheduler(testConfig(2, SchedulerType::FCFS, 5, 1000, 1000, 0), 1, kSchedulerStore);
    scheduler.createProcess("a", 1024);
    scheduler.createProcess("b", 1024);
    scheduler.createProcess("c", 1024);
    scheduler.tick();
    Utilization usage = scheduler.utilization();
    CHECK(usage.coreCount == 2);
    CHECK(usage.coresUsed == 2);
    CHECK(usage.running.size() == 2);
    if (usage.running.size() == 2) {
        CHECK(usage.running[0].name == "a" && usage.running[0].core == 0);
        CHECK(usage.running[1].name == "b" && usage.running[1].core == 1);
    }
    CHECK(usage.finished.empty());
}

TEST(batchGenerationCreatesAProcessEveryFrequencyTicks) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 1000, 1000, 0, 3), 1, kSchedulerStore);
    CHECK(scheduler.startBatchGeneration());
    CHECK(!scheduler.startBatchGeneration());
    for (uint64_t i = 0; i < 9 * kTicksPerBatchCycle; ++i) {
        scheduler.tick();
    }
    CHECK(scheduler.view("p01").has_value());
    CHECK(scheduler.view("p02").has_value());
    CHECK(scheduler.view("p03").has_value());
    CHECK(!scheduler.view("p04").has_value());
    CHECK(scheduler.stopBatchGeneration());
    CHECK(!scheduler.stopBatchGeneration());
    for (uint64_t i = 0; i < 30 * kTicksPerBatchCycle; ++i) {
        scheduler.tick();
    }
    CHECK(!scheduler.view("p04").has_value());
}

TEST(batchNamesSkipTakenNamesAndGrowPastTwoDigits) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 1000, 1000, 0, 1), 1, kSchedulerStore);
    scheduler.createProcess("p01", 1024);
    scheduler.startBatchGeneration();
    for (uint64_t i = 0; i < 100 * kTicksPerBatchCycle; ++i) {
        scheduler.tick();
    }
    CHECK(scheduler.view("p02").has_value());
    CHECK(scheduler.view("p100").has_value());
    CHECK(scheduler.view("p101").has_value());
    CHECK(!scheduler.view("p102").has_value());
}

TEST(rolledMemorySizesArePowersOfTwoInRange) {
    Config config = testConfig(1, SchedulerType::FCFS, 5, 1000, 1000, 0);
    config.minMemPerProc = 64;
    config.maxMemPerProc = 1024;
    Scheduler scheduler(config, 1, kSchedulerStore);
    std::set<uint64_t> sizes;
    for (int i = 0; i < 500; ++i) {
        sizes.insert(scheduler.rollMemorySize());
    }
    CHECK(sizes == std::set<uint64_t>({64, 128, 256, 512, 1024}));
}

TEST(batchProcessesGetRolledMemory) {
    Config config = testConfig(1, SchedulerType::FCFS, 5, 1000, 1000, 0, 1);
    config.minMemPerProc = 128;
    config.maxMemPerProc = 256;
    Scheduler scheduler(config, 1, kSchedulerStore);
    scheduler.startBatchGeneration();
    for (uint64_t i = 0; i < 40 * kTicksPerBatchCycle; ++i) {
        scheduler.tick();
    }
    bool inRange = true;
    std::set<uint64_t> sizes;
    for (int i = 1; i <= 40; ++i) {
        std::optional<ProcessView> view = scheduler.view(std::string(i < 10 ? "p0" : "p") + std::to_string(i));
        inRange = inRange && view && (view->memoryBytes == 128 || view->memoryBytes == 256);
        if (view) {
            sizes.insert(view->memoryBytes);
        }
    }
    CHECK(inRange);
    CHECK(sizes.size() == 2);
}

TEST(everyProcessFinishesAfterExactlyItsLength) {
    Scheduler scheduler(testConfig(4, SchedulerType::RR, 2, 1, 60, 0, 1), 3, kSchedulerStore);
    scheduler.startBatchGeneration();
    for (uint64_t i = 0; i < 200 * kTicksPerBatchCycle; ++i) {
        scheduler.tick();
    }
    scheduler.stopBatchGeneration();
    for (int i = 0; i < 200000 && scheduler.utilization().finished.size() < 200; ++i) {
        scheduler.tick();
    }
    Utilization usage = scheduler.utilization();
    CHECK(usage.finished.size() == 200);
    bool exact = true;
    for (const ProcessRow& row : usage.finished) {
        exact = exact && row.executed == row.total && row.core == -1;
    }
    CHECK(exact);
    CHECK(usage.coresUsed == 0);
    CHECK(usage.running.empty());
    CHECK(scheduler.memoryReport().usedBytes == 0);
}

TEST(noProcessRunsOnTwoCoresAtOnce) {
    Scheduler scheduler(testConfig(128, SchedulerType::RR, 2, 1, 50, 0, 1), 5, kSchedulerStore);
    scheduler.startBatchGeneration();
    bool consistent = true;
    for (int i = 0; i < 1500; ++i) {
        if (i == 1000) {
            scheduler.stopBatchGeneration();
        }
        scheduler.tick();
        Utilization usage = scheduler.utilization();
        std::set<std::string> names;
        int previousCore = -1;
        for (const ProcessRow& row : usage.running) {
            names.insert(row.name);
            consistent = consistent && row.core > previousCore && row.core < 128;
            previousCore = row.core;
        }
        consistent = consistent && names.size() == usage.running.size() &&
                     usage.coresUsed == usage.running.size() && usage.coreCount == 128;
    }
    CHECK(consistent);
}

TEST(fcfsNeverPreemptsARunningProcess) {
    // With a long delay, a preempted process would leave while busy-waiting, without having run
    // an instruction. Under FCFS a process only leaves right after running SLEEP or its last line.
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 2, 20, 20, 50), 9, kSchedulerStore);
    scheduler.createProcess("a", 1024);
    scheduler.createProcess("b", 1024);
    scheduler.createProcess("c", 1024);
    std::string previous;
    uint64_t previousExecuted = 0;
    bool neverPreempted = true;
    for (int i = 0; i < 20000 && scheduler.utilization().finished.size() < 3; ++i) {
        scheduler.tick();
        Utilization usage = scheduler.utilization();
        std::string current = onFirstCore(usage);
        if (!previous.empty() && current != previous) {
            std::optional<ProcessView> view = scheduler.view(previous);
            neverPreempted = neverPreempted && view && view->executed > previousExecuted;
        }
        previous = current;
        previousExecuted = executedOnFirstCore(usage);
    }
    CHECK(neverPreempted);
    CHECK(scheduler.utilization().finished.size() == 3);
}

TEST(roundRobinPreemptsAfterTheQuantum) {
    // A process may keep the core past its quantum only while the other one sleeps (at most
    // 10 ticks), so with two busy processes no run lasts much longer than the quantum.
    Scheduler scheduler(testConfig(1, SchedulerType::RR, 3, 20, 20, 50), 9, kSchedulerStore);
    scheduler.createProcess("a", 1024);
    scheduler.createProcess("b", 1024);
    std::string previous;
    uint64_t previousExecuted = 0;
    int run = 0;
    int longestRun = 0;
    int preemptions = 0;
    for (int i = 0; i < 20000 && scheduler.utilization().finished.size() < 2; ++i) {
        scheduler.tick();
        Utilization usage = scheduler.utilization();
        std::string current = onFirstCore(usage);
        run = (!current.empty() && current == previous) ? run + 1 : (current.empty() ? 0 : 1);
        if (usage.finished.empty()) {
            longestRun = std::max(longestRun, run);
        }
        if (!previous.empty() && current != previous) {
            std::optional<ProcessView> view = scheduler.view(previous);
            if (view && !view->finished && view->executed == previousExecuted) {
                ++preemptions;
            }
        }
        previous = current;
        previousExecuted = executedOnFirstCore(usage);
    }
    CHECK(longestRun <= 3 + 12);
    CHECK(preemptions > 0);
    CHECK(scheduler.utilization().finished.size() == 2);
}

TEST(delayPerExecSpacesInstructions) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 30, 30, 3), 4, kSchedulerStore);
    scheduler.createProcess("a", 1024);
    uint64_t lastExecuted = 0;
    int lastChange = -1000;
    bool spaced = true;
    for (int t = 0; t < 2000; ++t) {
        scheduler.tick();
        uint64_t executed = executedOf(scheduler, "a");
        if (executed != lastExecuted) {
            spaced = spaced && t - lastChange >= 4;
            lastChange = t;
            lastExecuted = executed;
        }
    }
    CHECK(spaced);
    CHECK(hasFinished(scheduler, "a"));
}

TEST(pageFaultsWaitForACoreAndCostATick) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 1, 1, 0), 1, kSchedulerStore);
    CHECK(scheduler.createProcess("p", 1024, {makeDeclare("x", 5), makePrint("x = ", "x")}));
    CHECK(scheduler.memoryReport().usedBytes == 0);
    scheduler.tick(); // p gets the core
    CHECK(executedOf(scheduler, "p") == 0 && scheduler.memoryReport().usedBytes == 0);
    scheduler.tick(); // page fault: page 0 is loaded and DECLARE waits
    MemoryReport report = scheduler.memoryReport();
    CHECK(executedOf(scheduler, "p") == 0);
    CHECK(report.usedBytes == 256 && report.pagedIn == 1 && report.pagedOut == 0);
    CHECK(report.processes.size() == 1 && report.processes[0].name == "p" && report.processes[0].bytes == 256);
    scheduler.tick(); // DECLARE runs
    CHECK(executedOf(scheduler, "p") == 1);
    scheduler.tick(); // PRINT runs without a fault: page 0 is still loaded
    std::optional<ProcessView> view = scheduler.view("p");
    CHECK(view && view->finished && !view->crashed && view->logs.size() == 1 && view->logs[0].text == "x = 5");
    CHECK(scheduler.memoryReport().usedBytes == 0); // a finished process frees its frames
}

TEST(specProgramFinishesWithResult15) {
    Scheduler scheduler(testConfig(4, SchedulerType::RR, 5, 1, 1, 0), 1, kSchedulerStore);
    std::optional<std::vector<Instruction>> program = parseProgram(kSpecProgram);
    CHECK(program.has_value());
    if (!program) {
        return;
    }
    CHECK(scheduler.createProcess("process2", 4096, *program));
    CHECK(!scheduler.createProcess("process2", 4096, *program));
    tickUntilFinished(scheduler, "process2", 100);
    std::optional<ProcessView> view = scheduler.view("process2");
    CHECK(view && view->finished && view->executed == 6 && view->total == 6 && view->memoryBytes == 4096);
    CHECK(view && !view->logs.empty() && view->logs.back().text == "Result: 15");
}

TEST(processesFinishWhenMemoryIsScarce) {
    // 2 frames of 64 bytes for 3 processes whose WRITE and READ each need 2 pages at once.
    Config config = testConfig(2, SchedulerType::RR, 2, 1, 1, 0);
    config.maxOverallMem = 128;
    config.memPerFrame = 64;
    Scheduler scheduler(config, 1, kSchedulerStore);
    const char* const names[] = {"a", "b", "c"};
    for (const char* name : names) {
        CHECK(scheduler.createProcess(name, 256,
                                      {makeDeclare("x", 7), makeWrite(0x80, variableOperand("x")), makeRead("y", 0x80),
                                       makePrint("y = ", "y")}));
    }
    for (int i = 0; i < 5000 && scheduler.utilization().finished.size() < 3; ++i) {
        scheduler.tick();
    }
    CHECK(scheduler.utilization().finished.size() == 3);
    for (const char* name : names) {
        std::optional<ProcessView> view = scheduler.view(name);
        CHECK(view && !view->crashed && !view->logs.empty() && view->logs.back().text == "y = 7");
    }
    MemoryReport report = scheduler.memoryReport();
    CHECK(report.pagedOut > 0 && report.pagedIn > report.pagedOut);
    CHECK(report.usedBytes == 0);
}

TEST(backingStoreFileIsRewrittenEveryTwentyTicks) {
    // 1 frame of 64 bytes: the WRITE to page 1 (tick 4) evicts page 0, which holds x = 1.
    Config config = testConfig(1, SchedulerType::FCFS, 5, 1, 1, 0);
    config.maxOverallMem = 64;
    config.memPerFrame = 64;
    Scheduler scheduler(config, 1, kSchedulerStore);
    std::vector<Instruction> program = {makeDeclare("x", 1), makeWrite(0x40, literal(5))};
    for (int i = 0; i < 30; ++i) {
        program.push_back(makePrint("waiting"));
    }
    CHECK(scheduler.createProcess("a", 128, program));
    for (uint64_t i = 0; i < kTicksPerBackingStoreWrite; ++i) {
        scheduler.tick();
    }
    CHECK(scheduler.memoryReport().pagedOut == 1);
    CHECK(fileLines(kSchedulerStore).size() == 1); // evicted, but the file isn't due yet
    scheduler.tick();                              // tick 21 rewrites the file
    std::vector<std::string> lines = fileLines(kSchedulerStore);
    CHECK(lines.size() == 2 && lines[1].rfind("a (ID 1) page 0: 01 00 ", 0) == 0);
}

TEST(memoryAccessViolationShutsTheProcessDown) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 1, 1, 0), 1, kSchedulerStore);
    CHECK(scheduler.createProcess("f", 64,
                                  {makeDeclare("x", 1), makeWrite(0x500, variableOperand("x")), makePrint("never")}));
    std::time_t before = std::time(nullptr);
    tickUntilFinished(scheduler, "f", 100);
    std::time_t after = std::time(nullptr);
    std::optional<ProcessView> view = scheduler.view("f");
    CHECK(view && view->finished && view->crashed);
    CHECK(view && view->faultAddress == 0x500 && view->crashedAt >= before && view->crashedAt <= after);
    CHECK(view && view->executed == 1 && view->total == 3 && view->logs.empty());
    CHECK(scheduler.memoryReport().usedBytes == 0);
    Utilization usage = scheduler.utilization();
    CHECK(usage.finished.size() == 1);
    if (usage.finished.size() == 1) {
        CHECK(usage.finished[0].name == "f" && usage.finished[0].executed == 1 && usage.finished[0].total == 3);
    }
}

TEST(memoryReportCountsCoreTicksAndProcessMemory) {
    Scheduler scheduler(testConfig(2, SchedulerType::FCFS, 5, 1, 1, 0), 1, kSchedulerStore);
    std::vector<Instruction> program;
    for (int i = 0; i < 20; ++i) {
        program.push_back(makeDeclare("x", static_cast<uint16_t>(i)));
    }
    CHECK(scheduler.createProcess("p", 1024, program));
    for (int i = 0; i < 10; ++i) {
        scheduler.tick();
    }
    MemoryReport report = scheduler.memoryReport();
    CHECK(report.coreCount == 2 && report.coresUsed == 1);
    CHECK(report.activeTicks == 9 && report.idleTicks == 11); // p is on core 0 from the second tick
    CHECK(report.totalBytes == 16384 && report.usedBytes == 256);
    uint64_t listed = 0;
    for (const ProcessMemory& row : report.processes) {
        listed += row.bytes;
    }
    CHECK(listed == report.usedBytes);
    CHECK(executedOf(scheduler, "p") == 8); // one page fault tick, then one DECLARE per tick
}

TEST(backgroundThreadRunsTicksUntilStopped) {
    Scheduler scheduler(testConfig(1, SchedulerType::FCFS, 5, 1000000, 1000000, 0), 1, kSchedulerStore);
    scheduler.createProcess("a", 1024);
    scheduler.start();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (executedOf(scheduler, "a") < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    scheduler.stop();
    uint64_t executed = executedOf(scheduler, "a");
    CHECK(executed >= 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(executedOf(scheduler, "a") == executed);
}

int main() {
    for (const Test& test : registry()) {
        currentTest = test.name;
        test.run();
    }
    std::cout << registry().size() << " tests, " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
