#include "MemoryManager.h"

#include <algorithm>
#include <utility>

MemoryManager::MemoryManager(uint64_t totalBytes, uint64_t frameBytes, std::string backingStorePath)
    : frameSize(frameBytes), physical(totalBytes), frames(totalBytes / frameBytes),
      backingStore(std::move(backingStorePath)) {
    // Hand out frame 0 first.
    for (size_t frame = frames.size(); frame > 0; --frame) {
        freeFrames.push_back(frame - 1);
    }
}

void MemoryManager::allocate(int id, const std::string& name, uint64_t memoryBytes) {
    if (tables.count(id) > 0) {
        return;
    }
    uint64_t pageCount = std::max<uint64_t>(1, (memoryBytes + frameSize - 1) / frameSize);
    tables.emplace(id, PageTable{name, std::vector<int64_t>(pageCount, -1)});
}

bool MemoryManager::isAllocated(int id) const {
    return tables.count(id) > 0;
}

bool MemoryManager::isResident(int id, uint64_t page) const {
    auto found = tables.find(id);
    return found != tables.end() && page < found->second.frameOf.size() && found->second.frameOf[page] >= 0;
}

bool MemoryManager::pageIn(int id, const std::vector<uint64_t>& pages) {
    auto found = tables.find(id);
    if (found == tables.end()) {
        return false;
    }
    PageTable& table = found->second;
    auto needed = [&pages](uint64_t page) { return std::find(pages.begin(), pages.end(), page) != pages.end(); };

    std::vector<uint64_t> missing;
    for (uint64_t page : pages) {
        if (page >= table.frameOf.size()) {
            return false;
        }
        if (table.frameOf[page] < 0 && std::find(missing.begin(), missing.end(), page) == missing.end()) {
            missing.push_back(page);
        }
    }

    // Frames that can hold the missing pages: free ones, and unpinned ones holding none of these pages.
    size_t available = freeFrames.size();
    for (size_t frame : loadOrder) {
        if (!frames[frame].pinned && !(frames[frame].owner == id && needed(frames[frame].page))) {
            ++available;
        }
    }
    if (available < missing.size()) {
        return false;
    }

    // Pin the resident pages first so that loading the others can't evict them.
    for (uint64_t page : pages) {
        if (table.frameOf[page] >= 0) {
            frames[static_cast<size_t>(table.frameOf[page])].pinned = true;
        }
    }
    for (uint64_t page : missing) {
        load(id, table, page, takeFrame());
    }
    return true;
}

void MemoryManager::unpin(int id) {
    auto found = tables.find(id);
    if (found == tables.end()) {
        return;
    }
    for (int64_t frame : found->second.frameOf) {
        if (frame >= 0) {
            frames[static_cast<size_t>(frame)].pinned = false;
        }
    }
}

uint16_t MemoryManager::read16(int id, uint64_t address) const {
    std::optional<size_t> low = physicalAddress(id, address);
    std::optional<size_t> high = physicalAddress(id, address + 1);
    return static_cast<uint16_t>((low ? physical[*low] : 0) | (high ? physical[*high] : 0) << 8);
}

void MemoryManager::write16(int id, uint64_t address, uint16_t value) {
    if (std::optional<size_t> low = physicalAddress(id, address)) {
        physical[*low] = static_cast<uint8_t>(value & 0xFF);
    }
    if (std::optional<size_t> high = physicalAddress(id, address + 1)) {
        physical[*high] = static_cast<uint8_t>(value >> 8);
    }
}

void MemoryManager::release(int id) {
    auto found = tables.find(id);
    if (found == tables.end()) {
        return;
    }
    loadOrder.erase(std::remove_if(loadOrder.begin(), loadOrder.end(),
                                   [this, id](size_t frame) { return frames[frame].owner == id; }),
                    loadOrder.end());
    for (int64_t frame : found->second.frameOf) {
        if (frame >= 0) {
            frames[static_cast<size_t>(frame)] = Frame();
            freeFrames.push_back(static_cast<size_t>(frame));
        }
    }
    tables.erase(found);
    backingStore.removeProcess(id);
}

void MemoryManager::flush() {
    backingStore.flush();
}

uint64_t MemoryManager::frameBytes() const {
    return frameSize;
}

uint64_t MemoryManager::totalBytes() const {
    return frames.size() * frameSize;
}

uint64_t MemoryManager::usedBytes() const {
    return loadOrder.size() * frameSize;
}

uint64_t MemoryManager::residentBytes(int id) const {
    auto found = tables.find(id);
    if (found == tables.end()) {
        return 0;
    }
    const std::vector<int64_t>& frameOf = found->second.frameOf;
    return std::count_if(frameOf.begin(), frameOf.end(), [](int64_t frame) { return frame >= 0; }) * frameSize;
}

uint64_t MemoryManager::pagedIn() const {
    return pagesIn;
}

uint64_t MemoryManager::pagedOut() const {
    return pagesOut;
}

std::optional<size_t> MemoryManager::physicalAddress(int id, uint64_t address) const {
    auto found = tables.find(id);
    uint64_t page = address / frameSize;
    if (found == tables.end() || page >= found->second.frameOf.size() || found->second.frameOf[page] < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(found->second.frameOf[page]) * frameSize + address % frameSize;
}

size_t MemoryManager::takeFrame() {
    if (!freeFrames.empty()) {
        size_t frame = freeFrames.back();
        freeFrames.pop_back();
        return frame;
    }
    // pageIn() has checked that an unpinned frame exists.
    auto victim = std::find_if(loadOrder.begin(), loadOrder.end(), [this](size_t frame) { return !frames[frame].pinned; });
    size_t frame = *victim;
    loadOrder.erase(victim);

    Frame& evicted = frames[frame];
    PageTable& owner = tables.at(evicted.owner);
    auto start = physical.begin() + static_cast<std::ptrdiff_t>(frame * frameSize);
    backingStore.store(evicted.owner, owner.name, evicted.page,
                       std::vector<uint8_t>(start, start + static_cast<std::ptrdiff_t>(frameSize)));
    owner.frameOf[evicted.page] = -1;
    evicted = Frame();
    ++pagesOut;
    return frame;
}

void MemoryManager::load(int id, PageTable& table, uint64_t page, size_t frame) {
    auto start = physical.begin() + static_cast<std::ptrdiff_t>(frame * frameSize);
    std::optional<std::vector<uint8_t>> saved = backingStore.take(id, page);
    if (saved) {
        std::copy(saved->begin(), saved->end(), start);
    } else {
        std::fill(start, start + static_cast<std::ptrdiff_t>(frameSize), uint8_t{0});
    }
    frames[frame] = Frame{id, page, true};
    table.frameOf[page] = static_cast<int64_t>(frame);
    loadOrder.push_back(frame);
    ++pagesIn;
}
