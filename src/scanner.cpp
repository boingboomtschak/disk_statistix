#include "scanner.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <unordered_map>

std::wstring Tree::Name(uint32_t i) const {
    const Node& n = nodes[i];
    return std::wstring(&names[n.nameOff], n.nameLen);
}

std::wstring Tree::Path(uint32_t i) const {
    std::vector<uint32_t> chain;
    for (uint32_t n = i; n != NONE; n = nodes[n].parent) chain.push_back(n);
    std::wstring p;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        p.append(&names[nodes[*it].nameOff], nodes[*it].nameLen);
    }
    return p;
}

Scanner::~Scanner() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void Scanner::Start(const std::wstring& root) {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    done_ = false;
    files = dirs = bytes = errors = 0;
    result_.reset();

    ULARGE_INTEGER avail{}, total{}, totalFree{};
    bool isDrive = root.size() <= 3 && root.size() >= 2 && root[1] == L':';
    expectedBytes = (isDrive && GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &totalFree))
                        ? total.QuadPart - totalFree.QuadPart
                        : 0;
    worker_ = std::thread([this, root] { Run(root); });
}

std::unique_ptr<Tree> Scanner::Take() {
    if (worker_.joinable()) worker_.join();
    return std::move(result_);
}

std::wstring Scanner::CurrentPath() {
    std::lock_guard lock(mutex_);
    return current_;
}

static std::wstring LongPathPattern(const std::wstring& path) {
    std::wstring p = path.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
    if (p.back() != L'\\') p += L'\\';
    p += L'*';
    return p;
}

void Scanner::Run(std::wstring root) {
    auto t0 = std::chrono::steady_clock::now();
    auto lastReport = t0;
    if (root.size() == 2 && root[1] == L':') root += L'\\';

    auto tree = std::make_unique<Tree>();
    Tree& t = *tree;
    t.rootPath = root;
    ULARGE_INTEGER avail{}, total{}, totalFree{};
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &totalFree)) {
        t.diskTotal = total.QuadPart;
        t.diskFree = totalFree.QuadPart;
    }

    Node rootNode;
    rootNode.isDir = 1;
    rootNode.nameLen = (uint16_t)root.size();
    t.names.assign(root.begin(), root.end());
    t.nodes.push_back(rootNode);

    std::unordered_map<std::wstring, uint32_t> extIds;
    std::vector<uint32_t> stack{0};
    std::wstring ext;
    WIN32_FIND_DATAW fd;

    // Depth-first walk. Each folder's entries are appended as one contiguous block.
    while (!stack.empty()) {
        if (cancel_) {
            done_ = true;
            return;
        }
        uint32_t d = stack.back();
        stack.pop_back();
        std::wstring path = t.Path(d);

        auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::milliseconds(50)) {
            lastReport = now;
            std::lock_guard lock(mutex_);
            current_ = path;
        }

        HANDLE h = FindFirstFileExW(LongPathPattern(path).c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            t.errorList.push_back({path, code});
            errors++;
            continue;
        }
        uint32_t first = (uint32_t)t.nodes.size();
        do {
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
            bool dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            if (dir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;  // junctions double count

            size_t len = wcslen(name);
            Node n;
            n.parent = d;
            n.nameOff = (uint32_t)t.names.size();
            n.nameLen = (uint16_t)len;
            t.names.insert(t.names.end(), name, name + len);
            if (dir) {
                n.isDir = 1;
                dirs++;
            } else {
                n.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                n.files = 1;
                const wchar_t* dot = wcsrchr(name, L'.');
                if (dot && dot != name && dot[1]) {
                    ext.assign(dot);
                    for (auto& c : ext) c = (wchar_t)towlower(c);
                } else {
                    ext = L"(none)";
                }
                auto [it, inserted] = extIds.try_emplace(ext, (uint32_t)t.exts.size());
                if (inserted) t.exts.push_back({ext});
                n.ext = it->second;
                t.exts[n.ext].bytes += n.size;
                t.exts[n.ext].count++;
                files++;
                bytes += n.size;
            }
            t.nodes.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        t.nodes[d].first = first;
        t.nodes[d].count = (uint32_t)t.nodes.size() - first;
        for (uint32_t i = (uint32_t)t.nodes.size(); i-- > first;)
            if (t.nodes[i].isDir) stack.push_back(i);
    }

    // Children always have higher indices than their parent, so one reverse
    // pass rolls sizes up the tree.
    for (size_t i = t.nodes.size(); i-- > 1;) {
        Node& p = t.nodes[t.nodes[i].parent];
        p.size += t.nodes[i].size;
        p.files += t.nodes[i].files;
    }
    for (Node& n : t.nodes)
        if (n.count > 1)
            std::sort(t.nodes.begin() + n.first, t.nodes.begin() + n.first + n.count,
                      [](const Node& a, const Node& b) { return a.size > b.size; });
    // Sorting moved nodes within their sibling blocks; re-point children at them.
    for (uint32_t i = 0; i < t.nodes.size(); i++)
        for (uint32_t c = t.nodes[i].first; c < t.nodes[i].first + t.nodes[i].count; c++)
            t.nodes[c].parent = i;

    t.files = files;
    t.dirs = dirs;
    t.errors = errors;
    t.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    result_ = std::move(tree);
    done_ = true;
}
