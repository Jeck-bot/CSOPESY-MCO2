#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// First line of the backing store file.
inline constexpr char kBackingStoreHeader[] = "CSOPESY backing store: pages swapped out of main memory";

// The pages swapped out of main memory. They are kept here and mirrored in a text file that can be
// opened at any time, one line per page:
//   <process name> (ID <id>) page <page number>: <page bytes as two-digit hex, space-separated>
class BackingStore {
public:
    // Creates the file at path, or empties it if it exists.
    explicit BackingStore(std::string path);

    // Saves a page evicted from main memory.
    void store(int id, const std::string& name, uint64_t page, std::vector<uint8_t> bytes);

    // Removes and returns a saved page, or returns nullopt if it isn't saved.
    std::optional<std::vector<uint8_t>> take(int id, uint64_t page);

    // Removes every saved page of a process.
    void removeProcess(int id);

    // Rewrites the file if pages were saved or removed since it was last written.
    void flush();

    size_t pageCount() const;

private:
    struct SavedPage {
        std::string name;
        std::vector<uint8_t> bytes;
    };

    std::string path;
    std::map<std::pair<int, uint64_t>, SavedPage> pages; // keyed by (process ID, page number)
    bool changed = true;                                 // the file needs rewriting
};
