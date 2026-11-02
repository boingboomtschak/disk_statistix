#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "scanner.h"

struct DupGroup {
    std::vector<uint32_t> members;  // node indices; groups shrink when copies are deleted
    uint64_t size = 0;              // size of one copy
    bool isDir = false;
};

struct DupResult {
    std::vector<uint32_t> groupOf;  // per node: index into groups, or NONE
    std::vector<DupGroup> groups;   // sorted by reclaimable space, largest first
};

// Finds files and folders with identical content while reading as little as possible:
//   1. no I/O: files are bucketed by size, folders by a structural signature
//      (multiset of the sizes beneath them); anything unique is ruled out.
//   2. hash the first 16 KB of the remaining candidates (multi-threaded).
//   3. fully hash only files that still collide.
//   4. folders match when the multiset of their children's content hashes matches.
class DupFinder {
public:
    enum Stage { Grouping, Partial, Full, Matching, Finished };

    ~DupFinder();
    void Start(const Tree* tree, uint64_t minSize, bool folders);
    void Cancel() { cancel_ = true; }
    bool Done() const { return done_; }
    bool Running() const { return worker_.joinable() && !done_; }
    std::unique_ptr<DupResult> Take();  // null if cancelled

    std::atomic<int> stage{Grouping};
    std::atomic<uint64_t> filesDone{0}, filesTotal{0}, bytesDone{0}, bytesTotal{0};

private:
    void Run(const Tree* tree, uint64_t minSize, bool folders);
    bool HashFiles(const Tree& t, const std::vector<uint32_t>& work, uint64_t limit, std::vector<uint64_t>& out);

    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::unique_ptr<DupResult> result_;
};
