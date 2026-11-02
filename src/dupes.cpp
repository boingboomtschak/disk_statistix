#include "dupes.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

// ---------------------------------------------------------------- hashing (XXH64-style)

static constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                          P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;

static inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t Read64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t Round(uint64_t acc, uint64_t in) { return Rotl(acc + in * P2, 31) * P1; }
static inline uint64_t Merge(uint64_t acc, uint64_t v) { return (acc ^ Round(0, v)) * P1 + P4; }

static uint64_t Mix(uint64_t h) {
    h ^= h >> 33; h *= P2;
    h ^= h >> 29; h *= P3;
    return h ^ (h >> 32);
}

static uint64_t Hash(const uint8_t* p, size_t n, uint64_t seed) {
    const uint8_t* end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        for (; p + 32 <= end; p += 32) {
            v1 = Round(v1, Read64(p));
            v2 = Round(v2, Read64(p + 8));
            v3 = Round(v3, Read64(p + 16));
            v4 = Round(v4, Read64(p + 24));
        }
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = Merge(Merge(Merge(Merge(h, v1), v2), v3), v4);
    } else {
        h = seed + P5;
    }
    h += n;
    for (; p + 8 <= end; p += 8) h = Rotl(h ^ Round(0, Read64(p)), 27) * P1 + P4;
    for (; p < end; p++) h = Rotl(h ^ (*p * P5), 11) * P1;
    return Mix(h);
}

// Order-independent combination, so a folder's hash doesn't depend on sibling order.
static inline uint64_t Combine(uint64_t acc, uint64_t child) { return acc + Mix(child ^ P3); }

// ---------------------------------------------------------------- finder

DupFinder::~DupFinder() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void DupFinder::Start(const Tree* tree, uint64_t minSize, bool folders) {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    done_ = false;
    result_.reset();
    stage = Grouping;
    filesDone = filesTotal = bytesDone = bytesTotal = 0;
    worker_ = std::thread([=, this] { Run(tree, minSize, folders); });
}

std::unique_ptr<DupResult> DupFinder::Take() {
    if (worker_.joinable()) worker_.join();
    return std::move(result_);
}

static std::wstring LongPath(const std::wstring& p) {
    return p.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + p.substr(2) : L"\\\\?\\" + p;
}

// Hash up to `limit` bytes of each file in `work` (0 = whole file) on several threads.
// Unreadable files get a hash derived from their index so they never match anything.
bool DupFinder::HashFiles(const Tree& t, const std::vector<uint32_t>& work, uint64_t limit, std::vector<uint64_t>& out) {
    constexpr size_t kChunk = 1 << 20;
    std::atomic<size_t> next{0};
    auto worker = [&] {
        std::vector<uint8_t> buf(kChunk);
        for (size_t w; (w = next++) < work.size() && !cancel_;) {
            uint32_t i = work[w];
            uint64_t want = limit ? std::min<uint64_t>(limit, t.nodes[i].size) : t.nodes[i].size;
            uint64_t h = 0, got = 0;
            HANDLE f = CreateFileW(LongPath(t.Path(i)).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                while (got < want && !cancel_) {
                    DWORD n = 0;
                    DWORD ask = (DWORD)std::min<uint64_t>(kChunk, want - got);
                    if (!ReadFile(f, buf.data(), ask, &n, nullptr) || n == 0) break;
                    h = Hash(buf.data(), n, h);  // chain per chunk; chunk boundaries are fixed offsets
                    got += n;
                    bytesDone += n;
                }
                CloseHandle(f);
            }
            out[i] = got == want ? Mix(h ^ want) : Mix(~(uint64_t)i * P5);
            filesDone++;
        }
    };
    unsigned n = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < n; k++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    return !cancel_;
}

template <class Key>
static void KeepCollisions(std::vector<uint32_t>& items, Key key) {
    std::unordered_map<uint64_t, uint32_t> count;
    count.reserve(items.size());
    for (uint32_t i : items) count[key(i)]++;
    std::erase_if(items, [&](uint32_t i) { return count[key(i)] < 2; });
}

void DupFinder::Run(const Tree* tree, uint64_t minSize, bool folders) {
    const Tree& t = *tree;
    const std::vector<Node>& N = t.nodes;
    const size_t n = N.size();
    auto finish = [&] { done_ = true; };

    // --- 1. Grouping (no I/O)
    // Structural signature: a folder's sig combines its children's sigs; a file's is its size.
    // Children always have higher indices than their parent, so a reverse pass works bottom-up.
    std::vector<uint64_t> sig(n);
    for (size_t i = n; i-- > 0;) {
        const Node& x = N[i];
        if (!x.isDir) { sig[i] = Mix(x.size + P1); continue; }
        uint64_t h = x.count * P4;
        for (uint32_t c = x.first; c < x.first + x.count; c++) h = Combine(h, sig[c]);
        sig[i] = Mix(h ^ x.size);
    }
    if (cancel_) return finish();

    std::vector<uint32_t> candDirs;
    if (folders) {
        for (uint32_t i = 1; i < n; i++)
            if (N[i].isDir && N[i].files && N[i].size && N[i].size >= minSize) candDirs.push_back(i);
        KeepCollisions(candDirs, [&](uint32_t i) { return sig[i]; });
    }

    // Files worth hashing: big enough and sharing a size, or inside a candidate folder.
    std::vector<uint8_t> need(n, 0);
    for (uint32_t i = 0; i < n; i++)
        if (!N[i].isDir && N[i].size && N[i].size >= minSize) need[i] = 1;
    {
        std::vector<uint32_t> stack(candDirs);
        while (!stack.empty()) {
            uint32_t d = stack.back();
            stack.pop_back();
            for (uint32_t c = N[d].first; c < N[d].first + N[d].count; c++) {
                if (N[c].isDir) { if (need[c] != 2) { need[c] = 2; stack.push_back(c); } }
                else need[c] = 1;
            }
        }
    }
    std::vector<uint32_t> work;
    for (uint32_t i = 0; i < n; i++)
        if (need[i] == 1 && N[i].size) work.push_back(i);
    KeepCollisions(work, [&](uint32_t i) { return N[i].size; });  // a unique size can't have a twin
    if (cancel_) return finish();

    // content[i]: equal values <=> equal content. Default: unique per node.
    std::vector<uint64_t> content(n);
    for (uint32_t i = 0; i < n; i++) content[i] = N[i].isDir || N[i].size ? Mix(~(uint64_t)i * P2) : Mix(P3);  // empty files are all alike

    // --- 2. Partial hash: first 16 KB
    constexpr uint64_t kHead = 16 * 1024;
    stage = Partial;
    filesTotal = work.size();
    bytesTotal = 0;
    for (uint32_t i : work) bytesTotal += std::min<uint64_t>(kHead, N[i].size);
    std::vector<uint64_t> head(n);
    if (!HashFiles(t, work, kHead, head)) return finish();
    for (uint32_t i : work) content[i] = Mix(head[i] ^ N[i].size);

    // --- 3. Full hash for files larger than the head that still collide
    std::erase_if(work, [&](uint32_t i) { return N[i].size <= kHead; });
    KeepCollisions(work, [&](uint32_t i) { return content[i]; });
    stage = Full;
    filesDone = 0;
    bytesDone = 0;
    filesTotal = work.size();
    bytesTotal = 0;
    for (uint32_t i : work) bytesTotal += N[i].size;
    std::vector<uint64_t> full(n);
    if (!HashFiles(t, work, 0, full)) return finish();
    for (uint32_t i : work) content[i] = Mix(content[i] ^ full[i] * P1);

    // --- 4. Matching
    stage = Matching;
    for (size_t i = n; i-- > 0;) {
        if (!N[i].isDir) continue;
        uint64_t h = N[i].count * P4;
        for (uint32_t c = N[i].first; c < N[i].first + N[i].count; c++) h = Combine(h, content[c]);
        content[i] = Mix(h ^ N[i].size);
    }

    auto res = std::make_unique<DupResult>();
    res->groupOf.assign(n, NONE);
    std::unordered_map<uint64_t, uint32_t> byKey;
    auto addGroups = [&](const std::vector<uint32_t>& items, bool isDir) {
        byKey.clear();
        for (uint32_t i : items) {
            uint64_t key = content[i] ^ Mix(N[i].size + (isDir ? P5 : 0));
            auto [it, inserted] = byKey.try_emplace(key, (uint32_t)res->groups.size());
            if (inserted) res->groups.push_back({{}, N[i].size, isDir});
            res->groups[it->second].members.push_back(i);
        }
    };
    std::vector<uint32_t> files;
    for (uint32_t i = 0; i < n; i++)
        if (!N[i].isDir && N[i].size && N[i].size >= minSize) files.push_back(i);
    addGroups(candDirs, true);
    addGroups(files, false);
    std::erase_if(res->groups, [](const DupGroup& g) { return g.members.size() < 2; });

    // Hide groups that are implied by a duplicate folder (every copy sits inside one).
    std::vector<uint8_t> inDupDir(n, 0);
    for (const DupGroup& g : res->groups)
        if (g.isDir)
            for (uint32_t m : g.members) inDupDir[m] = 1;
    std::erase_if(res->groups, [&](const DupGroup& g) {
        return std::all_of(g.members.begin(), g.members.end(), [&](uint32_t m) { return inDupDir[N[m].parent]; });
    });

    std::sort(res->groups.begin(), res->groups.end(), [](const DupGroup& a, const DupGroup& b) {
        return a.size * (a.members.size() - 1) > b.size * (b.members.size() - 1);
    });
    for (uint32_t g = 0; g < res->groups.size(); g++)
        for (uint32_t m : res->groups[g].members) res->groupOf[m] = g;

    stage = Finished;
    result_ = std::move(res);
    done_ = true;
}
