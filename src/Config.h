#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <string>

enum class SchedulerType { FCFS, RR };

// Settings read from config.txt by the "initialize" command.
// Defaults are the spec's sample values. Ranges up to 2^32 need 64-bit fields.
struct Config {
    uint32_t numCpu = 4;                         // num-cpu            [1, 128]
    SchedulerType scheduler = SchedulerType::RR; // scheduler          "fcfs" or "rr"
    uint64_t quantumCycles = 5;                  // quantum-cycles     [1, 2^32], rr only
    uint64_t batchProcessFreq = 1;               // batch-process-freq [1, 2^32]
    uint64_t minIns = 1000;                      // min-ins            [1, 2^32]
    uint64_t maxIns = 2000;                      // max-ins            [1, 2^32]
    uint64_t delayPerExec = 0;                   // delay-per-exec     [0, 2^32]

    // Memory sizes in bytes. Each is a power of 2 from 64 to 65536 (see isValidMemorySize).
    uint64_t maxOverallMem = 16384;              // max-overall-mem    total main memory
    uint64_t memPerFrame = 256;                  // mem-per-frame      frame and page size
    uint64_t minMemPerProc = 4096;               // min-mem-per-proc   memory of scheduler-start
    uint64_t maxMemPerProc = 4096;               // max-mem-per-proc   processes, rolled in [min, max]
};

// True if bytes is a power of 2 from 64 (2^6) to 65536 (2^16), the spec's range for every memory size.
bool isValidMemorySize(uint64_t bytes);

// A memory size written in decimal, or nullopt if it isn't a valid memory size.
std::optional<uint64_t> parseMemorySize(const std::string& text);

// Reads whitespace-separated "key value" pairs into config. Keys that are missing keep the
// defaults above. Returns false and sets error, leaving config unchanged, if a value is invalid.
bool parseConfig(std::istream& input, Config& config, std::string& error);

// Reads the config file at path with parseConfig.
bool loadConfig(const std::string& path, Config& config, std::string& error);
