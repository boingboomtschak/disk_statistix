#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr uint32_t NONE = UINT32_MAX;

// Compact node: every file and folder on the drive gets one. Children of a
// folder are stored contiguously at [first, first + count), sorted by size desc.
struct Node {
    uint64_t size = 0;
    uint32_t parent = NONE;
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t nameOff = 0;
    uint32_t ext = NONE;   // index into Tree::exts, NONE for folders
    uint32_t files = 0;    // number of files at or below this node
    uint16_t nameLen = 0;
    uint8_t isDir = 0;
};

struct ExtInfo {
    std::wstring ext;
    uint64_t bytes = 0;
    uint64_t count = 0;
};

struct ScanError {
    std::wstring path;
    uint32_t code;  // GetLastError()
};

struct Tree {
    std::wstring rootPath;
    std::vector<Node> nodes;  // nodes[0] is the root
    std::vector<wchar_t> names;
    std::vector<ExtInfo> exts;
    uint64_t files = 0, dirs = 0, errors = 0;
    std::vector<ScanError> errorList;  // folders that couldn't be listed
    uint64_t diskTotal = 0, diskFree = 0;
    double seconds = 0;

    std::wstring Name(uint32_t i) const;
    std::wstring Path(uint32_t i) const;
};

class Scanner {
public:
    ~Scanner();
    void Start(const std::wstring& root);
    void Cancel() { cancel_ = true; }
    bool Done() const { return done_; }
    bool Running() const { return worker_.joinable() && !done_; }
    std::unique_ptr<Tree> Take();  // null if cancelled
    std::wstring CurrentPath();

    std::atomic<uint64_t> files{0}, dirs{0}, bytes{0}, errors{0};
    uint64_t expectedBytes = 0;  // used space of the drive, 0 when scanning a folder

private:
    void Run(std::wstring root);

    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::mutex mutex_;
    std::wstring current_;
    std::unique_ptr<Tree> result_;
};
