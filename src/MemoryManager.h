#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "BackingStore.h"

// Demand-paged main memory. It is split into frames of frameBytes; each process's virtual memory is
// split into pages of the same size, loaded into frames only when needed. When no frame is free,
// the frame loaded longest ago (FIFO) that isn't pinned is evicted to the backing store.
//
// A page is pinned from the page fault that needs it until the faulting instruction runs (unpin),
// so cores competing for frames can't evict each other's pages before they are used.
class MemoryManager {
public:
    // totalBytes and frameBytes are valid memory sizes with frameBytes <= totalBytes.
    MemoryManager(uint64_t totalBytes, uint64_t frameBytes, std::string backingStorePath);
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // Gives a process a page table for memoryBytes of virtual memory, with no page loaded.
    // Does nothing if the process already has one.
    void allocate(int id, const std::string& name, uint64_t memoryBytes);
    bool isAllocated(int id) const;
    bool isResident(int id, uint64_t page) const;

    // Makes every listed page of the process resident and pins it. A missing page is read back
    // from the backing store, or zero-filled if it was never evicted. All or nothing: if too few
    // frames are free or unpinned, nothing changes and false is returned.
    bool pageIn(int id, const std::vector<uint64_t>& pages);

    // Unpins every page of the process.
    void unpin(int id);

    // The little-endian uint16 at a virtual address. The pages holding both bytes must be resident;
    // bytes that aren't read as 0 and aren't written.
    uint16_t read16(int id, uint64_t address) const;
    void write16(int id, uint64_t address, uint16_t value);

    // Frees the process's frames and its pages in the backing store.
    void release(int id);

    // Writes the backing store file if it changed.
    void flush();

    uint64_t frameBytes() const;
    uint64_t totalBytes() const;
    uint64_t usedBytes() const;          // bytes of all frames in use
    uint64_t residentBytes(int id) const; // bytes of the process's frames
    uint64_t pagedIn() const;            // pages loaded into frames so far
    uint64_t pagedOut() const;           // pages evicted to the backing store so far

private:
    struct Frame {
        int owner = -1; // process ID, or -1 when free
        uint64_t page = 0;
        bool pinned = false;
    };

    struct PageTable {
        std::string name;
        std::vector<int64_t> frameOf; // frame of each page, or -1 when not resident
    };

    std::optional<size_t> physicalAddress(int id, uint64_t address) const;
    size_t takeFrame(); // a free frame, evicting the oldest unpinned one if none is free
    void load(int id, PageTable& table, uint64_t page, size_t frame);

    uint64_t frameSize;
    std::vector<uint8_t> physical;
    std::vector<Frame> frames;
    std::vector<size_t> freeFrames;
    std::deque<size_t> loadOrder; // frames in use, loaded longest ago first
    std::unordered_map<int, PageTable> tables;
    BackingStore backingStore;
    uint64_t pagesIn = 0;
    uint64_t pagesOut = 0;
};
