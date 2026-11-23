#include "Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "Utils.h"

namespace {

constexpr uint64_t kMaxValue = 4294967296ULL; // 2^32, the largest value the spec allows

// True for 1 to 19 decimal digits, which always fit in a uint64_t.
bool isSmallNumber(const std::string& text) {
    return !text.empty() && text.size() <= 19 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Parses a decimal number within [low, high] into value.
bool parseNumber(const std::string& key, const std::string& text, uint64_t low, uint64_t high,
                 uint64_t& value, std::string& error) {
    if (isSmallNumber(text)) {
        value = std::stoull(text);
        if (value >= low && value <= high) {
            return true;
        }
    }
    error = "Invalid " + key + " \"" + text + "\": must be from " + std::to_string(low) + " to " +
            std::to_string(high) + ".";
    return false;
}

// Parses a memory size (see isValidMemorySize) into value.
bool parseMemorySize(const std::string& key, const std::string& text, uint64_t& value, std::string& error) {
    if (isSmallNumber(text) && isValidMemorySize(std::stoull(text))) {
        value = std::stoull(text);
        return true;
    }
    error = "Invalid " + key + " \"" + text + "\": must be a power of 2 from 64 to 65536.";
    return false;
}

std::string withoutQuotes(const std::string& text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

} // namespace

bool isValidMemorySize(uint64_t bytes) {
    return bytes >= 64 && bytes <= 65536 && (bytes & (bytes - 1)) == 0;
}

bool parseConfig(std::istream& input, Config& config, std::string& error) {
    Config result;
    std::string key;
    while (input >> key) {
        std::string value;
        if (!(input >> value)) {
            error = "Missing value for " + key + ".";
            return false;
        }
        key = toLower(key);

        uint64_t number = 0;
        if (key == "num-cpu") {
            if (!parseNumber(key, value, 1, 128, number, error)) {
                return false;
            }
            result.numCpu = static_cast<uint32_t>(number);
        } else if (key == "scheduler") {
            std::string name = toLower(withoutQuotes(value));
            if (name == "fcfs") {
                result.scheduler = SchedulerType::FCFS;
            } else if (name == "rr") {
                result.scheduler = SchedulerType::RR;
            } else {
                error = "Invalid scheduler \"" + value + "\": must be \"fcfs\" or \"rr\".";
                return false;
            }
        } else if (key == "quantum-cycles") {
            if (!parseNumber(key, value, 1, kMaxValue, result.quantumCycles, error)) {
                return false;
            }
        } else if (key == "batch-process-freq") {
            if (!parseNumber(key, value, 1, kMaxValue, result.batchProcessFreq, error)) {
                return false;
            }
        } else if (key == "min-ins") {
            if (!parseNumber(key, value, 1, kMaxValue, result.minIns, error)) {
                return false;
            }
        } else if (key == "max-ins") {
            if (!parseNumber(key, value, 1, kMaxValue, result.maxIns, error)) {
                return false;
            }
        } else if (key == "delay-per-exec" || key == "delays-per-exec") {
            // The spec's table spells it "delays-per-exec"; its sample file uses "delay-per-exec".
            if (!parseNumber(key, value, 0, kMaxValue, result.delayPerExec, error)) {
                return false;
            }
        } else if (key == "max-overall-mem") {
            if (!parseMemorySize(key, value, result.maxOverallMem, error)) {
                return false;
            }
        } else if (key == "mem-per-frame") {
            if (!parseMemorySize(key, value, result.memPerFrame, error)) {
                return false;
            }
        } else if (key == "min-mem-per-proc") {
            if (!parseMemorySize(key, value, result.minMemPerProc, error)) {
                return false;
            }
        } else if (key == "max-mem-per-proc" || key == "max-mem-proc") {
            // The spec's table names it "max-mem-per-proc"; its description calls it "max-mem-proc".
            if (!parseMemorySize(key, value, result.maxMemPerProc, error)) {
                return false;
            }
        }
        // Unknown keys are ignored.
    }

    if (result.minIns > result.maxIns) {
        error = "min-ins (" + std::to_string(result.minIns) + ") cannot be greater than max-ins (" +
                std::to_string(result.maxIns) + ").";
        return false;
    }
    if (result.memPerFrame > result.maxOverallMem) {
        error = "mem-per-frame (" + std::to_string(result.memPerFrame) + ") cannot be greater than max-overall-mem (" +
                std::to_string(result.maxOverallMem) + ").";
        return false;
    }
    if (result.minMemPerProc > result.maxMemPerProc) {
        error = "min-mem-per-proc (" + std::to_string(result.minMemPerProc) +
                ") cannot be greater than max-mem-per-proc (" + std::to_string(result.maxMemPerProc) + ").";
        return false;
    }
    config = result;
    return true;
}

bool loadConfig(const std::string& path, Config& config, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "Could not open " + path + ".";
        return false;
    }
    return parseConfig(file, config, error);
}
