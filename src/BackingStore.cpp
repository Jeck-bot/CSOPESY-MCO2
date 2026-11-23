#include "BackingStore.h"

#include <fstream>

BackingStore::BackingStore(std::string path) : path(std::move(path)) {
    flush();
}

void BackingStore::store(int id, const std::string& name, uint64_t page, std::vector<uint8_t> bytes) {
    pages[{id, page}] = SavedPage{name, std::move(bytes)};
    changed = true;
}

std::optional<std::vector<uint8_t>> BackingStore::take(int id, uint64_t page) {
    auto found = pages.find({id, page});
    if (found == pages.end()) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes = std::move(found->second.bytes);
    pages.erase(found);
    changed = true;
    return bytes;
}

void BackingStore::removeProcess(int id) {
    auto first = pages.lower_bound({id, 0});
    auto last = pages.lower_bound({id + 1, 0});
    if (first != last) {
        pages.erase(first, last);
        changed = true;
    }
}

void BackingStore::flush() {
    if (!changed) {
        return;
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        return; // try again at the next flush
    }
    const char* const hexDigits = "0123456789abcdef";
    file << kBackingStoreHeader << "\n";
    std::string line;
    for (const auto& [key, page] : pages) {
        line = page.name + " (ID " + std::to_string(key.first) + ") page " + std::to_string(key.second) + ":";
        line.reserve(line.size() + page.bytes.size() * 3 + 1);
        for (uint8_t byte : page.bytes) {
            line += ' ';
            line += hexDigits[byte >> 4];
            line += hexDigits[byte & 0x0F];
        }
        line += '\n';
        file << line;
    }
    changed = false;
}

size_t BackingStore::pageCount() const {
    return pages.size();
}
