#define IMGUI_DEFINE_MATH_OPERATORS
#include "app.h"

#include "theme.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cwctype>

// ---------------------------------------------------------------- helpers

static std::string U8(const wchar_t* s, size_t n) {
    if (!n) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s, (int)n, nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, out.data(), len, nullptr, nullptr);
    return out;
}
static std::string U8(const std::wstring& s) { return U8(s.data(), s.size()); }

static std::string FmtBytes(uint64_t b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 5) { v /= 1024; i++; }
    char buf[32];
    if (i == 0) snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    else snprintf(buf, sizeof buf, i >= 3 ? "%.2f %s" : "%.1f %s", v, units[i]);
    return buf;
}

static std::string FmtCount(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

// "1 folder", "2 folders"
static std::string Plural(uint64_t n, const char* word) {
    return FmtCount(n) + " " + word + (n == 1 ? "" : "s");
}

static std::string FmtPct(double x) {
    char buf[32];
    snprintf(buf, sizeof buf, x < 0.001 ? "%.3f%%" : "%.2f%%", x * 100);
    return buf;
}

using theme::Rgb;
using theme::Vec;

// Blend two 0xRRGGBB colours; t = 0 gives a, 1 gives b.
static ImU32 Mix(uint32_t a, uint32_t b, float t, int alpha = 255) {
    auto ch = [&](int s) { return (int)(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t + 0.5f); };
    return IM_COL32(ch(16), ch(8), ch(0), alpha);
}

// Trim the front of a string with "..." until it fits the width.
static std::string EllipsizeLeft(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    for (size_t i = 1; i < s.size(); i++) {
        if ((s[i] & 0xC0) == 0x80) continue;  // don't split UTF-8 sequences
        std::string t = "..." + s.substr(i);
        if (ImGui::CalcTextSize(t.c_str()).x <= width) return t;
    }
    return "...";
}

static void Shadowed(ImDrawList* dl, ImVec2 p, const std::string& s) {
    dl->AddText(p + ImVec2(1, 1), IM_COL32(0, 0, 0, 200), s.c_str());
    dl->AddText(p, Rgb(theme::kBrightWhite), s.c_str());
}

// Modals are pinned to the window centre every frame so they follow resizes, and
// shrink to fit narrow windows (height 0 = auto-fit).
// Draw tex over [a, b], cropped to fill it (CSS object-fit: cover).
static void DrawCover(ImDrawList* dl, const Texture& tex, ImVec2 a, ImVec2 b) {
    ImVec2 box = b - a;
    float k = std::max(box.x / tex.w, box.y / tex.h);
    ImVec2 seen(box.x / (tex.w * k), box.y / (tex.h * k));  // visible part of the texture, in UVs
    ImVec2 uv0 = (ImVec2(1, 1) - seen) * 0.5f;
    dl->AddImage((ImTextureID)(intptr_t)tex.srv, a, b, uv0, uv0 + seen);
}

// "1:05" / "1:02:03"
static std::string FmtTime(double sec) {
    int t = (int)std::max(0.0, sec), h = t / 3600, m = t / 60 % 60, s = t % 60;
    char buf[32];
    if (h) snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof buf, "%d:%02d", m, s);
    return buf;
}

static void CenterNextModal(float width) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    float s = ImGui::GetStyle().FontScaleDpi;
    if (width > 0) ImGui::SetNextWindowSize(ImVec2(std::min(width * s, vp->WorkSize.x - 32 * s), 0), ImGuiCond_Always);
}

// Settings that outlive the session, under HKCU\Software\Disk Statistix.
static constexpr const wchar_t* kSettingsKey = L"Software\\Disk Statistix";

static DWORD LoadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}

static void SaveSetting(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof value);
}

// Danger-coloured button for irreversible actions; pair with PopStyleColor(3).
static void PushDangerButton() {
    ImGui::PushStyleColor(ImGuiCol_Button, Vec(theme::kDanger));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Vec(theme::kDangerHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Vec(theme::kDangerActive));
}

// Extensions that get thumbnails (1) or a playing preview (2).
static int MediaKindOf(const std::wstring& ext) {
    static const wchar_t* kImages[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif", L".bmp", L".dib",
                                       L".tif", L".tiff", L".webp", L".heic", L".heif", L".avif", L".jxl", L".ico",
                                       L".dng", L".cr2", L".cr3", L".nef", L".arw", L".orf", L".rw2", L".raf"};
    static const wchar_t* kVideos[] = {L".mp4", L".m4v", L".mov", L".mkv", L".avi", L".wmv", L".webm",
                                       L".mpg", L".mpeg", L".ts", L".m2ts", L".mts", L".flv", L".3gp"};
    for (const wchar_t* e : kImages) if (ext == e) return 1;
    for (const wchar_t* e : kVideos) if (ext == e) return 2;
    return 0;
}

// ---------------------------------------------------------------- app

App::App(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx, const std::wstring& initialScan)
    : hwnd_(hwnd), ctx_(ctx), thumbs_(std::make_unique<ThumbCache>(device, 3)),
      big_(std::make_unique<ThumbCache>(device, 1)), video_(device) {
    showThumbs_ = LoadSetting(L"Thumbnails", 0) != 0;
    RefreshDrives();
    if (!initialScan.empty()) StartScan(initialScan);
}

void App::SetStatus(std::string msg, bool error) {
    status_ = std::move(msg);
    statusError_ = error;
    statusUntil_ = ImGui::GetTime() + 8;
}

void App::RefreshDrives() {
    drives_.clear();
    wchar_t buf[512];
    DWORD len = GetLogicalDriveStringsW(512, buf);
    for (wchar_t* d = buf; d < buf + len && *d; d += wcslen(d) + 1) {
        Drive drive;
        drive.root = d;
        switch (GetDriveTypeW(d)) {
        case DRIVE_FIXED: drive.type = "Local disk"; break;
        case DRIVE_REMOVABLE: drive.type = "Removable"; break;
        case DRIVE_REMOTE: drive.type = "Network"; break;
        case DRIVE_CDROM: drive.type = "Optical"; break;
        case DRIVE_RAMDISK: drive.type = "RAM disk"; break;
        default: continue;
        }
        ULARGE_INTEGER avail{}, total{}, totalFree{};
        if (!GetDiskFreeSpaceExW(d, &avail, &total, &totalFree)) continue;  // no media
        drive.total = total.QuadPart;
        drive.free = totalFree.QuadPart;
        wchar_t label[MAX_PATH + 1] = {}, fs[MAX_PATH + 1] = {};
        GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH);
        drive.label = U8(label, wcslen(label));
        if (fs[0]) drive.type += " (" + U8(fs, wcslen(fs)) + ")";
        drives_.push_back(drive);
    }
    if (driveSel_ >= (int)drives_.size()) driveSel_ = -1;
    if (driveSel_ < 0 && !drives_.empty()) driveSel_ = 0;
}

void App::StartScan(const std::wstring& path) {
    dupFinder_.Cancel();
    dupFinder_.Take();
    dups_.reset();
    lit_.clear();
    dupOpen_.clear();
    tree_.reset();  // free the old tree before building a new one
    rects_.clear();
    layoutRoot_ = NONE;
    sel_.clear();
    hovered_ = listHovered_ = ctxNode_ = clickNode_ = previewNode_ = previewRequest_ = NONE;
    video_.Stop();
    thumbs_->Clear();
    big_->Clear();
    scanTarget_ = path;
    scanner_.Start(path);
    mode_ = Mode::Scanning;
}

void App::OnTreeReady() {
    viewRoot_ = 0;
    history_ = {tree_->rootPath};
    histPos_ = 0;
    extOrder_.resize(tree_->exts.size());
    for (uint32_t i = 0; i < extOrder_.size(); i++) extOrder_[i] = i;
    std::sort(extOrder_.begin(), extOrder_.end(),
              [&](uint32_t a, uint32_t b) { return tree_->exts[a].bytes > tree_->exts[b].bytes; });
    extColor_.assign(tree_->exts.size(), theme::kOtherType);
    for (size_t i = 0; i < extOrder_.size() && i < std::size(theme::kPalette); i++)
        extColor_[extOrder_[i]] = theme::kPalette[i];
    extMedia_.resize(tree_->exts.size());
    for (size_t i = 0; i < extMedia_.size(); i++) extMedia_[i] = (uint8_t)MediaKindOf(tree_->exts[i].ext);
    SetWindowTextW(hwnd_, (L"Disk Statistix — " + tree_->rootPath).c_str());
}

void App::Zoom(uint32_t n) {
    if (n == NONE || !tree_) return;
    const Node& node = tree_->nodes[n];
    if (node.isDir && node.count) Navigate(n);
}

void App::Up() {
    if (tree_ && viewRoot_ != 0) Navigate(tree_->nodes[viewRoot_].parent);
}

// Show folder n and record it in the back/forward history (dropping anything ahead of here).
void App::Navigate(uint32_t n) {
    if (n == viewRoot_) return;
    viewRoot_ = n;
    history_.resize(histPos_ + 1);
    history_.push_back(tree_->Path(n));
    histPos_++;
}

// Step through the history like a browser (mouse back / forward). Folders deleted since are skipped.
void App::HistoryStep(int dir) {
    while (dir < 0 ? histPos_ > 0 : histPos_ + 1 < history_.size()) {
        size_t i = histPos_ + dir;
        uint32_t n = FindPath(history_[i]);
        if (n == 0 || (n != NONE && tree_->nodes[n].isDir && tree_->nodes[n].count)) {
            viewRoot_ = n;
            histPos_ = i;
            return;
        }
        history_.erase(history_.begin() + i);
        if (dir < 0) histPos_--;
    }
}

void App::OpenItem(uint32_t n) {
    std::wstring path = tree_->Path(n);
    // Default verb first; fall back to the "Open with" dialog for unassociated types.
    if ((INT_PTR)ShellExecuteW(hwnd_, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(hwnd_, L"openas", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::ShowInExplorer(uint32_t n) {
    std::wstring path = tree_->Path(n);
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

// Double-click: folders zoom in, pictures and videos open the preview, anything else opens in its app.
void App::Activate(uint32_t n, RectKind kind) {
    const Node& node = tree_->nodes[n];
    if (kind == Rest || (node.isDir && node.count)) Zoom(n);
    else if (MediaKind(n)) RequestPreview(n);
    else OpenItem(n);
}

void App::OpenContext(uint32_t n, bool rest) {
    ctxNode_ = n;
    ctxRest_ = rest;
    if (!rest && !IsSelected(n)) sel_ = {n};  // right-clicking inside a multi-selection keeps it
    ImGui::OpenPopup("ctx");
}

void App::Select(uint32_t n, bool toggle) {
    if (!toggle) sel_ = {n};
    else if (auto it = std::find(sel_.begin(), sel_.end(), n); it != sel_.end()) sel_.erase(it);
    else sel_.push_back(n);
}

int App::MediaKind(uint32_t n) const {
    const Node& node = tree_->nodes[n];
    return node.isDir || extMedia_.empty() ? 0 : extMedia_[node.ext];
}

void App::RequestPreview(uint32_t n) {
    if (n != NONE && MediaKind(n)) previewRequest_ = n;
}

// Room for the hover preview at the top of a tooltip.
ImVec2 App::TooltipBox() const {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    return ImVec2(vp->WorkSize.x * 0.45f, vp->WorkSize.y * 0.55f);
}

// Node for a full path, or NONE. Used to find items again after deletes reshuffle indices.
uint32_t App::FindPath(const std::wstring& path) const {
    const Tree& t = *tree_;
    if (path.size() < t.rootPath.size() || _wcsnicmp(path.c_str(), t.rootPath.c_str(), t.rootPath.size())) return NONE;
    uint32_t n = 0;
    size_t i = t.rootPath.size();
    while (i < path.size()) {
        if (path[i] == L'\\') { i++; continue; }
        size_t j = path.find(L'\\', i);
        if (j == std::wstring::npos) j = path.size();
        const Node& d = t.nodes[n];
        uint32_t found = NONE;
        for (uint32_t c = d.first; c < d.first + d.count && found == NONE; c++) {
            const Node& k = t.nodes[c];
            if (k.nameLen == j - i && !_wcsnicmp(&t.names[k.nameOff], &path[i], j - i)) found = c;
        }
        if (found == NONE) return NONE;
        n = found;
        i = j;
    }
    return n;
}

// Queue a delete confirmation; drops the root and anything already inside another chosen folder.
void App::RequestDelete(std::vector<uint32_t> nodes) {
    std::erase(nodes, 0u);
    auto inside = [&](uint32_t n) {
        for (uint32_t a = tree_->nodes[n].parent; a != NONE; a = tree_->nodes[a].parent)
            if (std::find(nodes.begin(), nodes.end(), a) != nodes.end()) return true;
        return false;
    };
    std::vector<uint32_t> keep;
    for (uint32_t n : nodes)
        if (!inside(n) && std::find(keep.begin(), keep.end(), n) == keep.end()) keep.push_back(n);
    std::sort(keep.begin(), keep.end(), [&](uint32_t a, uint32_t b) { return tree_->nodes[a].size > tree_->nodes[b].size; });
    deleteRequest_ = std::move(keep);
}

void App::SwapSiblings(uint32_t a, uint32_t b) {
    std::vector<Node>& nodes = tree_->nodes;
    std::swap(nodes[a], nodes[b]);
    for (uint32_t c = nodes[a].first; c < nodes[a].first + nodes[a].count; c++) nodes[c].parent = a;
    for (uint32_t c = nodes[b].first; c < nodes[b].first + nodes[b].count; c++) nodes[c].parent = b;
    if (viewRoot_ == a) viewRoot_ = b;
    else if (viewRoot_ == b) viewRoot_ = a;
    if (dups_) {
        uint32_t ga = dups_->groupOf[a], gb = dups_->groupOf[b];
        if (ga != NONE)
            for (uint32_t& m : dups_->groups[ga].members)
                if (m == a) m = b; else if (m == b && ga == gb) m = a;
        if (gb != NONE && gb != ga)
            for (uint32_t& m : dups_->groups[gb].members)
                if (m == b) m = a;
        std::swap(dups_->groupOf[a], dups_->groupOf[b]);
    }
}

// Drop a deleted node from the tree without rescanning: fix totals up the
// ancestor chain and keep every sibling block sorted by size.
void App::RemoveFromTree(uint32_t n) {
    Tree& t = *tree_;
    const Node removed = t.nodes[n];

    std::vector<uint32_t> stack{n};
    while (!stack.empty()) {
        uint32_t ki = stack.back();
        const Node& k = t.nodes[ki];
        stack.pop_back();
        if (uint32_t g = GroupOf(ki); g != NONE) {
            // Drop it from its duplicate group; a group with one copy left isn't a duplicate.
            auto& mem = dups_->groups[g].members;
            std::erase(mem, ki);
            dups_->groupOf[ki] = NONE;
            if (mem.size() < 2) {
                for (uint32_t m : mem) dups_->groupOf[m] = NONE;
                mem.clear();
            }
        }
        if (k.isDir) {
            t.dirs--;
            for (uint32_t c = k.first; c < k.first + k.count; c++) stack.push_back(c);
        } else {
            t.exts[k.ext].bytes -= k.size;
            t.exts[k.ext].count--;
            t.files--;
        }
    }

    for (uint32_t v = viewRoot_; v != NONE; v = t.nodes[v].parent)
        if (v == n) {  // was looking inside what got deleted: show its parent instead
            viewRoot_ = removed.parent;
            history_[histPos_] = t.Path(removed.parent);
            break;
        }

    // Move it to the end of its sibling block, then shrink the block.
    uint32_t p = removed.parent;
    uint32_t last = t.nodes[p].first + t.nodes[p].count - 1;
    for (uint32_t i = n; i < last; i++) SwapSiblings(i, i + 1);
    t.nodes[p].count--;

    for (uint32_t a = p; a != NONE;) {
        t.nodes[a].size -= removed.size;
        t.nodes[a].files -= removed.files;
        uint32_t gp = t.nodes[a].parent;
        if (gp != NONE) {
            uint32_t end = t.nodes[gp].first + t.nodes[gp].count;
            while (a + 1 < end && t.nodes[a + 1].size > t.nodes[a].size) {
                SwapSiblings(a, a + 1);
                a++;
            }
        }
        a = gp;
    }

    ULARGE_INTEGER avail{}, total{}, totalFree{};
    if (GetDiskFreeSpaceExW(t.rootPath.c_str(), &avail, &total, &totalFree)) t.diskFree = totalFree.QuadPart;
    sel_.clear();
    hovered_ = listHovered_ = ctxNode_ = clickNode_ = previewNode_ = previewRequest_ = NONE;
    hoverGroup_ = listHoverGroup_ = NONE;
    layoutRoot_ = NONE;
    RecomputeLit();
}

// ---------------------------------------------------------------- duplicates

static const struct { const char* label; uint64_t bytes; } kDupMins[] = {
    {"Any size", 0}, {"64 KB", 64ull << 10}, {"256 KB", 256ull << 10}, {"1 MB", 1ull << 20},
    {"10 MB", 10ull << 20}, {"100 MB", 100ull << 20}, {"1 GB", 1ull << 30}};

void App::RecomputeLit() {
    lit_.clear();
    if (!dups_) return;
    const auto& N = tree_->nodes;
    lit_.assign(N.size(), 0);
    lit_[0] = dups_->groupOf[0] != NONE;
    for (uint32_t i = 1; i < N.size(); i++)  // parents precede children
        lit_[i] = dups_->groupOf[i] != NONE || lit_[N[i].parent];
}

void App::DupOptionsModal() {
    if (dupOptionsRequest_) {
        dupOptionsRequest_ = false;
        ImGui::OpenPopup("Find duplicates");
    }
    CenterNextModal(0);
    if (!ImGui::BeginPopupModal("Find duplicates", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    float s = ImGui::GetStyle().FontScaleDpi;
    ImGui::TextUnformatted("Find files and folders with identical content.");
    ImGui::TextDisabled("Only files that share an exact size are read, and most are ruled out after 16 KB.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::BeginCombo("Smallest file to check", kDupMins[dupMinIdx_].label)) {
        for (int i = 0; i < (int)std::size(kDupMins); i++)
            if (ImGui::Selectable(kDupMins[i].label, i == dupMinIdx_)) dupMinIdx_ = i;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Also find duplicate folders", &dupFolders_);
    ImGui::Spacing();
    if (ImGui::Button("Start", ImVec2(110 * s, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        dups_.reset();
        lit_.clear();
        dupFinder_.Start(tree_.get(), kDupMins[dupMinIdx_].bytes, dupFolders_);
        dupScanning_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110 * s, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::DupScanModal() {
    if (dupScanning_ && !ImGui::IsPopupOpen("Finding duplicates")) ImGui::OpenPopup("Finding duplicates");
    CenterNextModal(560);
    if (!ImGui::BeginPopupModal("Finding duplicates", nullptr,
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    float s = ImGui::GetStyle().FontScaleDpi;
    static const char* kStage[] = {"Grouping by size...", "Checking the start of each candidate (16 KB)...",
                                   "Fully comparing remaining matches...", "Matching folders...", "Done"};
    int st = dupFinder_.stage;
    ImGui::TextUnformatted(kStage[st]);
    uint64_t bt = dupFinder_.bytesTotal, bd = dupFinder_.bytesDone;
    bool io = st == DupFinder::Partial || st == DupFinder::Full;
    std::string label = io ? FmtCount(dupFinder_.filesDone) + " / " + FmtCount(dupFinder_.filesTotal) + " files  ·  " +
                                 FmtBytes(bd) + " / " + FmtBytes(bt)
                           : "";
    ImGui::ProgressBar(io && bt ? (float)((double)bd / bt) : -(float)ImGui::GetTime(), ImVec2(-FLT_MIN, 0),
                       label.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(100 * s, 0))) dupFinder_.Cancel();

    if (dupFinder_.Done()) {
        dups_ = dupFinder_.Take();
        dupScanning_ = false;
        if (dups_) {
            showDups_ = !dups_->groups.empty();  // nothing to highlight: don't dim the whole map
            dupJustFound_ = true;
            dupOpen_.clear();
            RecomputeLit();
            uint64_t wasted = 0;
            for (const DupGroup& g : dups_->groups) wasted += g.size * (g.members.size() - 1);
            SetStatus(Plural(dups_->groups.size(), "duplicate group") + " found  ·  " + FmtBytes(wasted) +
                          " could be reclaimed", false);
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::DrawDupList() {
    Tree& t = *tree_;
    if (!dups_) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No duplicate scan yet. Use \"Find duplicates...\" in the toolbar.");
        ImGui::PopTextWrapPos();
        return;
    }
    uint64_t wasted = 0;
    size_t live = 0;
    for (const DupGroup& g : dups_->groups)
        if (g.members.size() >= 2) { wasted += g.size * (g.members.size() - 1); live++; }
    ImGui::Text("%s reclaimable", FmtBytes(wasted).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("in %s", Plural(live, "group").c_str());
    ImGui::Separator();

    // Flatten groups (+ members of expanded groups) into uniform-height rows so the
    // clipper only builds what's on screen, even with 100k groups.
    listHoverGroup_ = NONE;
    dupOpen_.resize(dups_->groups.size(), 0);
    struct Row { uint32_t group, member; };
    std::vector<Row> rows;
    rows.reserve(live);
    for (uint32_t g = 0; g < dups_->groups.size(); g++) {
        if (dups_->groups[g].members.size() < 2) continue;
        rows.push_back({g, NONE});
        if (dupOpen_[g])
            for (uint32_t m : dups_->groups[g].members) rows.push_back({g, m});
    }
    float indent = ImGui::GetStyle().IndentSpacing;
    if (ImGui::BeginChild("duplist")) {
        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const Row row = rows[r];
                const DupGroup& grp = dups_->groups[row.group];
                if (row.member == NONE) {
                    ImGui::PushID((int)row.group);
                    std::string reclaim = FmtBytes(grp.size * (grp.members.size() - 1));
                    float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                    ImGui::SetNextItemOpen(dupOpen_[row.group] != 0);
                    std::string head = U8(t.Name(grp.members[0])) + (grp.isDir ? "\\" : "") + "  ×" +
                                       std::to_string(grp.members.size());
                    dupOpen_[row.group] = ImGui::TreeNodeEx("##g", ImGuiTreeNodeFlags_SpanAvailWidth |
                                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen,
                                                            "%s", head.c_str());
                    if (ImGui::IsItemHovered()) {
                        listHoverGroup_ = row.group;
                        ImGui::SetTooltip("%s each  ·  %s reclaimable", FmtBytes(grp.size).c_str(), reclaim.c_str());
                    }
                    ImGui::SameLine(right - ImGui::CalcTextSize(reclaim.c_str()).x);
                    ImGui::TextDisabled("%s", reclaim.c_str());
                    ImGui::PopID();
                } else {
                    uint32_t m = row.member;
                    ImGui::PushID((int)m);
                    ImGui::Indent(indent);
                    std::string p = EllipsizeLeft(U8(t.Path(m)), ImGui::GetContentRegionAvail().x);
                    if (ImGui::Selectable(p.c_str(), IsSelected(m), ImGuiSelectableFlags_AllowDoubleClick)) {
                        bool ctrl = ImGui::GetIO().KeyCtrl;
                        if (!ctrl && t.nodes[m].parent != NONE) Zoom(t.nodes[m].parent);
                        Select(m, ctrl);
                        if (ImGui::IsMouseDoubleClicked(0)) Activate(m, Tile);
                    }
                    if (ImGui::IsItemHovered()) { listHovered_ = m; listHoverGroup_ = row.group; }
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8 * ImGui::GetStyle().FontScaleDpi)) dragRequest_ = m;
                    if (ImGui::IsItemClicked(1)) OpenContext(m);
                    ImGui::Unindent(indent);
                    ImGui::PopID();
                }
            }
        }
        ContextMenu();
    }
    ImGui::EndChild();
}

void App::DrawLogo(ImDrawList* dl, ImVec2 center, float size, float alpha) {
    // Option D, "block city": the treemap pushed up into isometric towers (logo/drafts.html).
    struct Poly { ImU32 rgb; float pts[8]; };
    static const Poly kPolys[] = {
        {0x2a2d36, {128, 196, 228, 146, 128, 96, 28, 146}},
        {0x2c62c9, {78, 121, 128, 146, 128, 86, 78, 61}},   {0x1c428c, {128, 146, 158, 131, 158, 71, 128, 86}},
        {0x8ab8ff, {78, 61, 128, 86, 158, 71, 108, 46}},    {0xc8382c, {158, 131, 188, 146, 188, 116, 158, 101}},
        {0x7d1d15, {188, 146, 218, 131, 218, 101, 188, 116}}, {0xff8f80, {158, 101, 188, 116, 218, 101, 188, 86}},
        {0x2f9c39, {98, 166, 128, 181, 128, 161, 98, 146}}, {0x1a5e21, {128, 181, 168, 161, 168, 141, 128, 161}},
        {0x9de88f, {98, 146, 128, 161, 168, 141, 138, 126}}, {0xd9a81f, {48, 146, 78, 161, 78, 146, 48, 131}},
        {0x7d600f, {78, 161, 98, 151, 98, 136, 78, 146}},   {0xffe27a, {48, 131, 78, 146, 98, 136, 68, 121}},
    };
    auto col = [&](uint32_t rgb) {
        return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(255 * alpha));
    };
    float k = size / 256.0f;
    auto at = [&](float x, float y) { return center + ImVec2((x - 128) * k, (y - 128) * k); };
    dl->AddRectFilled(at(8, 8), at(248, 248), col(0x14161c), 44 * k);
    for (const Poly& p : kPolys) {
        ImVec2 v[4];
        for (int i = 0; i < 4; i++) v[i] = at(p.pts[i * 2], p.pts[i * 2 + 1]);
        float area = 0;
        for (int i = 0; i < 4; i++) area += v[i].x * v[(i + 1) % 4].y - v[(i + 1) % 4].x * v[i].y;
        if (area < 0) std::swap(v[1], v[3]);  // ImGui's anti-aliasing wants clockwise winding
        dl->AddConvexPolyFilled(v, 4, col(p.rgb));
    }
}

void App::Delete(const std::vector<uint32_t>& nodes, bool permanent) {
    Tree& t = *tree_;
    std::vector<std::wstring> paths;
    uint64_t bytes = 0;
    for (uint32_t n : nodes) {
        paths.push_back(t.Path(n));
        bytes += t.nodes[n].size;
    }
    std::string what = nodes.size() == 1 ? U8(t.Name(nodes[0])) : FmtCount(nodes.size()) + " items";

    BOOL aborted = FALSE;
    IFileOperation* op = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
        // We already confirmed, so skip the shell's own prompt. FOF_WANTNUKEWARNING still
        // warns if something is too big for the Recycle Bin and would be destroyed instead.
        DWORD flags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR;
        if (!permanent) flags |= FOF_ALLOWUNDO | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE;
        op->SetOperationFlags(flags);
        op->SetOwnerWindow(hwnd_);
        bool queued = false;
        for (const std::wstring& p : paths) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                queued |= SUCCEEDED(op->DeleteItem(item, nullptr));
                item->Release();
            }
        }
        if (queued) op->PerformOperations();
        op->GetAnyOperationsAborted(&aborted);
        op->Release();
    }

    uint64_t freed = 0;
    size_t removed = RemoveMissing(paths, freed);
    if (removed == paths.size()) {
        SetStatus((permanent ? "Deleted " : "Moved to Recycle Bin: ") + what + " (" + FmtBytes(bytes) + ")", false);
    } else if (aborted) {
        SetStatus("Delete cancelled after " + FmtCount(removed) + " of " + FmtCount(paths.size()) +
                      " items. Partly removed folders won't update until you rescan.", true);
    } else {
        SetStatus("Deleted " + FmtCount(removed) + " of " + FmtCount(paths.size()) + " items (" + FmtBytes(freed) +
                      "). The rest are in use or need admin; rescan to refresh.", true);
    }
}

// Take items that no longer exist on disk out of the map. Indices shift with each removal, so each
// path is looked up again. Returns how many were removed and adds their size to `freed`.
size_t App::RemoveMissing(const std::vector<std::wstring>& paths, uint64_t& freed) {
    Tree& t = *tree_;
    size_t removed = 0;
    for (const std::wstring& p : paths) {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        uint32_t n = FindPath(p);
        if (n == NONE || n == 0) continue;
        freed += t.nodes[n].size;
        RemoveFromTree(n);
        removed++;
        std::erase_if(t.errorList, [&](const ScanError& e) {
            return e.path.size() >= p.size() && !_wcsnicmp(e.path.c_str(), p.c_str(), p.size()) &&
                   (e.path.size() == p.size() || e.path[p.size()] == L'\\');
        });
    }
    t.errors = t.errorList.size();
    return removed;
}

// Drag n (or the whole selection, if n is part of it) out to Explorer or another app. Uses the shell's
// own data object, so anything that takes files dragged from Explorer takes them from here too.
void App::DragOut(uint32_t n) {
    std::vector<uint32_t> items = IsSelected(n) && sel_.size() > 1 ? sel_ : std::vector<uint32_t>{n};
    std::vector<std::wstring> paths;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (uint32_t i : items) {
        paths.push_back(tree_->Path(i));
        if (PIDLIST_ABSOLUTE p = ILCreateFromPathW(paths.back().c_str())) pidls.push_back(p);
    }
    IShellItemArray* arr = nullptr;
    if (!pidls.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &arr))) {
        IDataObject* data = nullptr;
        if (SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
            video_.Stop();  // don't hold a file open that the drop target may want to move
            DWORD effect = 0;
            // Blocks until the drop (the shell draws the drag image and handles Esc / right-drag).
            SHDoDragDrop(hwnd_, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
            data->Release();
        }
        arr->Release();
    }
    for (PIDLIST_ABSOLUTE p : pidls) ILFree(p);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);  // the drag loop swallowed the release

    // A drop into another folder may have moved them away: update the map to match.
    uint64_t freed = 0;
    if (size_t moved = RemoveMissing(paths, freed)) {
        sel_.clear();
        SetStatus("Moved out of here: " + Plural(moved, "item") + " (" + FmtBytes(freed) + ")", false);
    }
}

std::wstring App::BrowseFolder() {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose a folder to scan");
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// ---------------------------------------------------------------- frame

void App::Frame() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    videoWanted_ = false;
    if (tree_ && mode_ == Mode::View) DrawView();
    else Blank();  // nothing to interact with behind the picker / scan dialogs
    ImGui::End();

    DiskModal();
    ScanModal();
    RescanModal();
    DeleteModal();
    ErrorsModal();
    PreviewModal();
    DupOptionsModal();
    DupScanModal();

    if (!videoWanted_ && !video_.Path().empty()) video_.Stop();  // preview tooltip / dialog went away
    if (dragRequest_ != NONE && tree_) DragOut(std::exchange(dragRequest_, NONE));
    video_.EndFrame();
    thumbs_->EndFrame();
    big_->EndFrame();
}

void App::Blank() {
    // Intentionally empty: while picking or scanning, the dialog is the only thing on screen.
}

void App::RescanModal() {
    if (rescanRequest_) {
        rescanRequest_ = false;
        ImGui::OpenPopup("Rescan");
    }
    CenterNextModal(0);
    if (!ImGui::BeginPopupModal("Rescan", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    float s = ImGui::GetStyle().FontScaleDpi;
    ImGui::Text("Rescan %s?", U8(tree_->rootPath).c_str());
    ImGui::TextDisabled("The current results will be cleared while it is scanned again.");
    ImGui::Spacing();
    if (ImGui::Button("Rescan", ImVec2(110 * s, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        std::wstring root = tree_->rootPath;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        StartScan(root);
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110 * s, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static std::string ErrorText(uint32_t code) {
    if (code == ERROR_ACCESS_DENIED) return "Access is denied";
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring w = n ? std::wstring(msg, n) : L"Error " + std::to_wstring(code);
    if (msg) LocalFree(msg);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L'.' || w.back() == L' ')) w.pop_back();
    return U8(w);
}

static bool IsSystemPath(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p) c = (wchar_t)towlower(c);
    if (p.size() <= 3) return true;  // a drive root
    for (const wchar_t* sys : {L":\\windows", L":\\program files", L":\\programdata", L":\\$recycle.bin",
                               L":\\system volume information", L":\\pagefile.sys", L":\\hiberfil.sys", L":\\swapfile.sys"})
        if (p.find(sys) == 1) return true;
    return false;
}

void App::DeleteModal() {
    if (!deleteRequest_.empty()) {
        deleteNodes_ = std::move(deleteRequest_);
        deleteRequest_.clear();
        ImGui::OpenPopup("Delete");
    }
    CenterNextModal(600);
    if (!ImGui::BeginPopupModal("Delete", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) return;
    float s = ImGui::GetStyle().FontScaleDpi;
    Tree& t = *tree_;
    uint64_t bytes = 0, files = 0;
    bool system = false;
    for (uint32_t n : deleteNodes_) {
        bytes += t.nodes[n].size;
        files += t.nodes[n].files;
        system |= IsSystemPath(t.Path(n));
    }

    ImGui::PushTextWrapPos(0);
    if (deleteNodes_.size() == 1) {
        uint32_t d = deleteNodes_[0];
        const Node& n = t.nodes[d];
        ImGui::TextUnformatted(n.isDir ? "Delete this folder and everything in it?" : "Delete this file?");
        ImGui::Spacing();
        ImGui::TextUnformatted(U8(t.Name(d)).c_str());
        ImGui::TextDisabled("%s", U8(t.Path(d)).c_str());
        if (n.isDir) ImGui::Text("%s  ·  %s files", FmtBytes(n.size).c_str(), FmtCount(n.files).c_str());
        else ImGui::Text("%s", FmtBytes(n.size).c_str());
    } else {
        ImGui::Text("Delete these %s items?", FmtCount(deleteNodes_.size()).c_str());
        ImGui::Text("%s  ·  %s files in total", FmtBytes(bytes).c_str(), FmtCount(files).c_str());
        ImGui::Spacing();
        float rowH = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginChild("items", ImVec2(0, rowH * std::min<size_t>(deleteNodes_.size(), 8) +
                                                    ImGui::GetStyle().WindowPadding.y * 2),
                              ImGuiChildFlags_Borders)) {
            for (uint32_t d : deleteNodes_) {
                std::string size = FmtBytes(t.nodes[d].size);
                float sizeW = ImGui::CalcTextSize(size.c_str()).x;
                float avail = ImGui::GetContentRegionAvail().x;
                ImGui::TextUnformatted(EllipsizeLeft(U8(t.Path(d)), avail - sizeW - 12 * s).c_str());
                ImGui::SameLine(avail - sizeW + ImGui::GetStyle().WindowPadding.x);
                ImGui::TextDisabled("%s", size.c_str());
            }
        }
        ImGui::EndChild();
    }
    if (system) {
        ImGui::Spacing();
        ImGui::TextColored(Vec(theme::kWarn),
                           "Warning: this includes a system location. Deleting it can break Windows or installed programs.");
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool close = false;
    if (ImGui::Button("Move to Recycle Bin", ImVec2(170 * s, 0))) {
        Delete(deleteNodes_, false);
        close = true;
    }
    ImGui::SameLine();
    PushDangerButton();
    if (ImGui::Button("Delete permanently...", ImVec2(170 * s, 0))) ImGui::OpenPopup("Delete permanently?");
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100 * s, 0)) ||
        (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("Delete permanently?")))
        close = true;
    ImGui::TextDisabled("Items in the Recycle Bin still use disk space until it is emptied.");
    ConfirmPermanentModal(close);
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Second, explicit yes/no before anything is destroyed. "No" is the default: Enter and Esc both cancel.
void App::ConfirmPermanentModal(bool& close) {
    CenterNextModal(0);
    if (!ImGui::BeginPopupModal("Delete permanently?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    float s = ImGui::GetStyle().FontScaleDpi;
    uint64_t bytes = 0;
    for (uint32_t n : deleteNodes_) bytes += tree_->nodes[n].size;
    std::string what = deleteNodes_.size() == 1 ? U8(tree_->Name(deleteNodes_[0]))
                                                : FmtCount(deleteNodes_.size()) + " items";
    ImGui::Text("Permanently delete %s (%s)?", what.c_str(), FmtBytes(bytes).c_str());
    ImGui::TextColored(Vec(theme::kError), "This skips the Recycle Bin and can't be undone.");
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button("No, keep it", ImVec2(150 * s, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    PushDangerButton();
    if (ImGui::Button("Yes, delete permanently", ImVec2(190 * s, 0))) {
        Delete(deleteNodes_, true);
        close = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndPopup();
}

// Folders the scan couldn't list (usually "Access is denied" without admin rights).
void App::ErrorsModal() {
    if (errorsRequest_) {
        errorsRequest_ = false;
        ImGui::OpenPopup("Unreadable folders");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float s = ImGui::GetStyle().FontScaleDpi;
    CenterNextModal(820);
    if (!ImGui::BeginPopupModal("Unreadable folders", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    const auto& errs = tree_->errorList;
    ImGui::PushTextWrapPos(0);
    ImGui::Text("%s couldn't be read, so %s contents aren't in the map.", Plural(errs.size(), "folder").c_str(),
                errs.size() == 1 ? "its" : "their");
    ImGui::TextDisabled("Most are protected system folders. Run Disk Statistix as administrator to include them.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    float rowH = ImGui::GetTextLineHeightWithSpacing();
    float listH = std::clamp(rowH * (float)(errs.size() + 1) + 8 * s, rowH * 4, vp->WorkSize.y * 0.8f - 170 * s);
    if (ImGui::BeginTable("errs", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable,
                          ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, 200 * s);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)errs.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const ScanError& e = errs[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                std::string path = U8(e.path);
                std::string shown = EllipsizeLeft(path, ImGui::GetContentRegionAvail().x);
                ImGui::Selectable(shown.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                if (shown.size() != path.size() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    ShellExecuteW(hwnd_, L"open", e.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (ImGui::BeginPopupContextItem("row")) {
                    if (ImGui::MenuItem("Open in Explorer"))
                        ShellExecuteW(hwnd_, L"open", e.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(path.c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", ErrorText(e.code).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Copy list", ImVec2(110 * s, 0))) {
        std::string all;
        for (const ScanError& e : errs) all += U8(e.path) + "\t" + ErrorText(e.code) + "\r\n";
        ImGui::SetClipboardText(all.c_str());
        SetStatus("Copied " + FmtCount(errs.size()) + " unreadable folder paths", false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110 * s, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    ImGui::TextDisabled("Double-click a folder to open it");
    ImGui::EndPopup();
}

void App::DiskModal() {
    if (mode_ == Mode::SelectDisk && !ImGui::IsPopupOpen("Select a disk to scan"))
        ImGui::OpenPopup("Select a disk to scan");

    float s = ImGui::GetStyle().FontScaleDpi;
    CenterNextModal(760);
    if (!ImGui::BeginPopupModal("Select a disk to scan", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) return;

    ImVec2 logoAt = ImGui::GetCursorScreenPos() + ImVec2(26 * s, 26 * s);
    DrawLogo(ImGui::GetWindowDrawList(), logoAt, 52 * s, 1.0f);
    ImGui::Indent(60 * s);
    ImGui::TextUnformatted("Disk Statistix");
    ImGui::TextDisabled("Choose a drive to see what's taking up space.");
    ImGui::Unindent(60 * s);
    ImGui::Dummy(ImVec2(0, 8 * s));

    std::wstring scan;
    if (ImGui::BeginTable("drives", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Drive", ImGuiTableColumnFlags_WidthFixed, 60 * s);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 140 * s);
        ImGui::TableSetupColumn("Used", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Free", ImGuiTableColumnFlags_WidthFixed, 90 * s);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)drives_.size(); i++) {
            const Drive& d = drives_[i];
            ImGui::PushID(i);
            ImGui::TableNextRow(0, ImGui::GetFrameHeight() + 4 * s);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable(U8(d.root).c_str(), driveSel_ == i,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, ImGui::GetFrameHeight()))) {
                driveSel_ = i;
                if (ImGui::IsMouseDoubleClicked(0)) scan = d.root;
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(d.label.empty() ? "(no label)" : d.label.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(d.type.c_str());
            ImGui::TableNextColumn();
            uint64_t used = d.total - d.free;
            std::string bar = FmtBytes(used) + " of " + FmtBytes(d.total);
            float frac = d.total ? (float)used / d.total : 0;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Vec(frac > 0.9f ? theme::kRed : theme::kBrightBlue));
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), bar.c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(FmtBytes(d.free).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (drives_.empty()) ImGui::TextDisabled("No drives found.");

    ImGui::Spacing();
    ImGui::BeginDisabled(driveSel_ < 0);
    if (ImGui::Button("Scan drive", ImVec2(120 * s, 0)) || (driveSel_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter)))
        scan = drives_[driveSel_].root;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Scan a folder...")) scan = BrowseFolder();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) RefreshDrives();
    if (tree_) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            mode_ = Mode::View;
            ImGui::CloseCurrentPopup();
        }
    }
    if (!scan.empty()) {
        StartScan(scan);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::ScanModal() {
    if (mode_ == Mode::Scanning && !ImGui::IsPopupOpen("Scanning"))
        ImGui::OpenPopup("Scanning");

    float s = ImGui::GetStyle().FontScaleDpi;
    CenterNextModal(640);
    if (!ImGui::BeginPopupModal("Scanning", nullptr,
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;

    ImGui::Text("Scanning %s", U8(scanTarget_).c_str());
    uint64_t bytes = scanner_.bytes;
    float frac = scanner_.expectedBytes ? std::min(1.0f, (float)((double)bytes / scanner_.expectedBytes))
                                        : -(float)ImGui::GetTime();  // indeterminate when scanning a folder
    std::string label = scanner_.expectedBytes ? FmtBytes(bytes) + " of ~" + FmtBytes(scanner_.expectedBytes)
                                               : FmtBytes(bytes);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), label.c_str());
    ImGui::Text("%s files  ·  %s folders  ·  %s unreadable", FmtCount(scanner_.files).c_str(),
                FmtCount(scanner_.dirs).c_str(), FmtCount(scanner_.errors).c_str());
    ImGui::TextDisabled("%s", EllipsizeLeft(U8(scanner_.CurrentPath()), ImGui::GetContentRegionAvail().x).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(100 * s, 0))) scanner_.Cancel();

    if (scanner_.Done()) {
        tree_ = scanner_.Take();
        if (tree_) {
            OnTreeReady();
            mode_ = Mode::View;
        } else {
            RefreshDrives();
            mode_ = Mode::SelectDisk;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- main view

void App::DrawView() {
    Tree& t = *tree_;
    float s = ImGui::GetStyle().FontScaleDpi;
    const Node& root = t.nodes[0];

    // Toolbar: actions + one-line summary + disk bar
    if (ImGui::Button("Choose disk...")) {
        RefreshDrives();
        mode_ = Mode::SelectDisk;
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) rescanRequest_ = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(histPos_ == 0);
    if (ImGui::Button("←")) HistoryStep(-1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back (mouse back button, Alt+Left)");
    ImGui::SameLine(0, 2 * s);
    ImGui::BeginDisabled(histPos_ + 1 >= history_.size());
    if (ImGui::Button("→")) HistoryStep(1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Forward (mouse forward button, Alt+Right)");
    ImGui::SameLine();
    ImGui::BeginDisabled(viewRoot_ == 0);
    if (ImGui::Button("Up")) Up();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Parent folder (Backspace)");
    ImGui::SameLine();
    if (ImGui::Button("Find duplicates...")) dupOptionsRequest_ = true;
    if (dups_) {
        ImGui::SameLine();
        ImGui::Checkbox("Highlight", &showDups_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dim everything that isn't a duplicate");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Thumbnails", &showThumbs_)) SaveSetting(L"Thumbnails", showThumbs_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show pictures and videos in their blocks; hover one for a bigger preview.\n"
                          "While duplicates are highlighted, only duplicates get thumbnails.");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    uint64_t used = t.diskTotal - t.diskFree;
    ImGui::Text("%s scanned (%s of disk)  ·  %s free of %s  ·  %s files", FmtBytes(root.size).c_str(),
                FmtPct(t.diskTotal ? (double)root.size / t.diskTotal : 0).c_str(), FmtBytes(t.diskFree).c_str(),
                FmtBytes(t.diskTotal).c_str(), FmtCount(t.files).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s folders, scanned in %.1fs", FmtCount(t.dirs).c_str(), t.seconds);
    if (t.errors) {
        ImGui::SameLine(0, 0);
        ImGui::Text("  ·  %s unreadable", FmtCount(t.errors).c_str());
        ImGui::SameLine(0, 4 * s);
        if (ImGui::SmallButton("?")) errorsRequest_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the folders that couldn't be read");
    }
    if (t.diskTotal) {
        ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(0, ImGui::GetFrameHeight() * 0.5f - 4 * s);
        ImVec2 sz(180 * s, 8 * s);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float a = std::min(1.0f, (float)root.size / t.diskTotal) * sz.x;
        float b = std::min(1.0f, (float)used / t.diskTotal) * sz.x;
        dl->AddRectFilled(p, p + sz, Rgb(theme::kRaised), 3 * s);
        dl->AddRectFilled(p, p + ImVec2(b, sz.y), Rgb(theme::kBlack), 3 * s);
        dl->AddRectFilled(p, p + ImVec2(a, sz.y), Rgb(theme::kBrightBlue), 3 * s);
        ImGui::Dummy(ImVec2(sz.x, ImGui::GetFrameHeight()));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scanned %s\nOther used %s (system, unreadable)\nFree %s", FmtBytes(root.size).c_str(),
                              FmtBytes(used > root.size ? used - root.size : 0).c_str(), FmtBytes(t.diskFree).c_str());
    }

    // Breadcrumb
    std::vector<uint32_t> chain;
    for (uint32_t n = viewRoot_; n != NONE; n = t.nodes[n].parent) chain.insert(chain.begin(), n);
    for (size_t i = 0; i < chain.size(); i++) {
        ImGui::PushID((int)i);
        if (i) {
            ImGui::SameLine(0, 4 * s);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0, 4 * s);
        }
        std::string name = U8(t.Name(chain[i]));
        if (i + 1 < chain.size()) {
            if (ImGui::SmallButton(name.c_str())) Navigate(chain[i]);
        } else {
            ImGui::TextUnformatted(name.c_str());
        }
        ImGui::PopID();
    }
    const Node& cur = t.nodes[viewRoot_];
    ImGui::SameLine();
    ImGui::TextDisabled("  %s  ·  %s of scan  ·  %s of disk  ·  %s files", FmtBytes(cur.size).c_str(),
                        FmtPct(root.size ? (double)cur.size / root.size : 0).c_str(),
                        FmtPct(t.diskTotal ? (double)cur.size / t.diskTotal : 0).c_str(), FmtCount(cur.files).c_str());

    // Map + side panel + status line
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float statusH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    float sideW = 320 * s;
    ImGui::BeginChild("map", ImVec2(avail.x - sideW - ImGui::GetStyle().ItemSpacing.x, avail.y - statusH),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawMap();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("side", ImVec2(0, avail.y - statusH));
    DrawSide();
    ImGui::EndChild();

    uint32_t info = hovered_ != NONE ? hovered_ : listHovered_ != NONE ? listHovered_ : sel_.size() == 1 ? sel_[0] : NONE;
    if (ImGui::GetTime() < statusUntil_) {
        ImGui::TextColored(Vec(statusError_ ? theme::kError : theme::kOk), "%s", status_.c_str());
    } else if (info == NONE && sel_.size() > 1) {
        uint64_t bytes = 0;
        for (uint32_t n : sel_) bytes += t.nodes[n].size;
        ImGui::Text("%s items selected  ·  %s", FmtCount(sel_.size()).c_str(), FmtBytes(bytes).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("Del to delete them  ·  Esc to clear the selection");
    } else if (info != NONE) {
        const Node& n = t.nodes[info];
        std::string line = FmtBytes(n.size) + "  ·  " + FmtPct(root.size ? (double)n.size / root.size : 0) +
                           " of scan  ·  ";
        ImGui::TextUnformatted(line.c_str());
        ImGui::SameLine(0, 0);
        ImGui::TextDisabled("%s", EllipsizeLeft(U8(t.Path(info)), ImGui::GetContentRegionAvail().x).c_str());
    } else {
        ImGui::TextDisabled("Click a folder heading to open it  ·  Ctrl+click to select several  ·  double-click to open / preview  ·  "
                            "right-click for more  ·  Del to delete  ·  Backspace to go up  ·  mouse back / forward for history");
    }

    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        bool alt = ImGui::GetIO().KeyAlt;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) Up();
        if (ImGui::IsMouseClicked(ImGuiMouseButton(3)) || (alt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) HistoryStep(-1);
        if (ImGui::IsMouseClicked(ImGuiMouseButton(4)) || (alt && ImGui::IsKeyPressed(ImGuiKey_RightArrow))) HistoryStep(1);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !sel_.empty()) RequestDelete(sel_);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) sel_.clear();
    }
}

App::Color App::NodeColor(uint32_t n) const {
    const Node& node = tree_->nodes[n];
    return node.isDir ? theme::kFolder : extColor_[node.ext];
}

void App::Relayout(ImVec2 size) {
    rects_.clear();
    headerH_ = ImGui::GetFontSize() + 4;
    LayoutNode(viewRoot_, 0, 0, size.x, size.y, 0);
    layoutRoot_ = viewRoot_;
    layoutSize_ = size;
}

void App::LayoutNode(uint32_t n, float x, float y, float w, float h, int depth) {
    if (w < 1 || h < 1) return;
    const Node& node = tree_->nodes[n];
    if (node.isDir && node.count && node.size && w >= 8 && h >= 8) {
        float top = (depth > 0 && w > 60 && h > headerH_ * 2.2f) ? headerH_ : 0;
        rects_.push_back({x, y, w, h, n, (uint16_t)depth, Folder, top});
        float pad = depth > 0 ? 1.0f : 0.0f;
        Squarify(n, x + pad, y + top + pad, w - 2 * pad, h - top - 2 * pad, depth + 1);
    } else {
        rects_.push_back({x, y, w, h, n, (uint16_t)depth, Tile, 0});
    }
}

void App::Squarify(uint32_t dir, float x, float y, float w, float h, int depth) {
    const Node& d = tree_->nodes[dir];
    if (w <= 0 || h <= 0 || !d.size) return;
    const std::vector<Node>& nodes = tree_->nodes;
    double scale = (double)w * h / d.size;
    uint64_t remaining = d.size;
    uint32_t i = d.first, end = d.first + d.count;
    auto area = [&](uint32_t k) { return nodes[k].size * scale; };

    while (i < end && nodes[i].size) {
        // Lump the long tail of tiny items into one block instead of laying out sub-pixel tiles.
        if (remaining * scale < 6.0 || w < 2 || h < 2) {
            rects_.push_back({x, y, w, h, dir, (uint16_t)depth, Rest, 0});
            return;
        }
        double side = std::min(w, h), side2 = side * side;
        double maxA = area(i), rowSum = 0, best = DBL_MAX;
        uint32_t j = i;
        while (j < end && nodes[j].size) {
            double a = area(j), rs = rowSum + a;
            double worst = std::max(side2 * maxA / (rs * rs), (rs * rs) / (side2 * a));
            if (worst > best) break;
            best = worst;
            rowSum = rs;
            j++;
        }
        if (w >= h) {
            float cw = (float)(rowSum / h), cy = y;
            for (uint32_t k = i; k < j; k++) {
                float ch = (float)(area(k) / cw);
                LayoutNode(k, x, cy, cw, ch, depth);
                cy += ch;
            }
            x += cw;
            w -= cw;
        } else {
            float ch = (float)(rowSum / w), cx = x;
            for (uint32_t k = i; k < j; k++) {
                float cw = (float)(area(k) / ch);
                LayoutNode(k, cx, y, cw, ch, depth);
                cx += cw;
            }
            y += ch;
            h -= ch;
        }
        for (uint32_t k = i; k < j; k++) remaining -= nodes[k].size;
        i = j;
    }
}

void App::DrawMap() {
    Tree& t = *tree_;
    float s = ImGui::GetStyle().FontScaleDpi;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    if (size.x < 10 || size.y < 10) return;
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hov = ImGui::IsItemHovered();
    // Pressed on an item and moved: drag it out of the app.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8 * s) && dragFrom_ != NONE) {
        dragRequest_ = dragFrom_;
        dragFrom_ = NONE;
    }
    if (layoutRoot_ != viewRoot_ || layoutSize_.x != size.x || layoutSize_.y != size.y) Relayout(size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p0 + size, true);
    dl->AddRectFilled(p0, p0 + size, Rgb(theme::kDeep));
    float fs = ImGui::GetFontSize();
    bool dim = showDups_ && !lit_.empty();
    float minThumb = 24 * s;

    for (const Rect& r : rects_) {
        ImVec2 a = p0 + ImVec2(r.x, r.y), b = a + ImVec2(r.w, r.h);
        if (r.kind == Folder) {
            dl->AddRectFilled(a, b, Rgb(theme::kDeep));
            if (r.header > 0) {
                ImU32 hc = dim && GroupOf(r.node) != NONE ? Rgb(theme::kDupHeader)  // duplicate folder
                                                          : Mix(theme::kBorder, theme::kSurface, std::min(1.0f, r.depth * 0.15f));
                dl->AddRectFilled(a, ImVec2(b.x, a.y + r.header), hc);
                std::string label = U8(t.Name(r.node)) + "   " + FmtBytes(t.nodes[r.node].size);
                dl->PushClipRect(a, ImVec2(b.x - 2, a.y + r.header), true);
                dl->AddText(a + ImVec2(4, (r.header - fs) * 0.5f), Rgb(theme::kForeground), label.c_str());
                dl->PopClipRect();
            }
        } else if (r.kind == Rest) {
            dl->AddRectFilled(a, b, dim ? Mix(theme::kRaised, theme::kDeep, 0.5f) : Rgb(theme::kRaised));
        } else if (dim && !lit_[r.node]) {
            dl->AddRectFilled(a, b, Mix(NodeColor(r.node), theme::kDeep, 0.8f));
            if (r.w > 3 && r.h > 3) dl->AddRect(a, b, IM_COL32(0, 0, 0, 110));
        } else {
            Color c = NodeColor(r.node);
            ImU32 tl = Mix(c, 0xFFFFFF, 0.35f), mid = Rgb(c), br = Mix(c, 0x000000, 0.62f);
            dl->AddRectFilledMultiColor(a, b, tl, mid, br, mid);
            if (r.w > 3 && r.h > 3) dl->AddRect(a, b, IM_COL32(0, 0, 0, 110));
            int media = showThumbs_ && r.w >= minThumb && r.h >= minThumb ? MediaKind(r.node) : 0;
            if (media) {
                // Fill the block inside its outline, cropping the picture to the block's shape.
                ImVec2 ia = a + ImVec2(1, 1), ib = b - ImVec2(1, 1), box = ib - ia;
                std::wstring path = t.Path(r.node);
                const Texture* tex = nullptr;
                float progress = -1;
                if (media == 2 && video_.Path() == path && (tex = video_.Frame(ctx_))) progress = video_.Progress();
                if (!tex) {
                    // Enough pixels that the picture's shorter side still covers the block once cropped.
                    int want = (int)std::max(box.x, box.y);
                    if (const Texture* have = thumbs_->Peek(path))
                        want = (int)(std::max(box.x / have->w, box.y / have->h) * std::max(have->w, have->h));
                    tex = thumbs_->Get(path, want, box.x * box.y);
                }
                if (tex) {
                    DrawCover(dl, *tex, ia, ib);
                    float g = std::min(box.x, box.y);
                    if (progress >= 0) {  // playing / being scrubbed in the block
                        float h = std::max(3.0f, 3 * s);
                        dl->AddRectFilled(ImVec2(ia.x, ib.y - h), ib, IM_COL32(0, 0, 0, 140));
                        dl->AddRectFilled(ImVec2(ia.x, ib.y - h), ImVec2(ia.x + box.x * progress, ib.y),
                                          Rgb(theme::kBrightYellow));
                    } else if (media == 2 && g > 28 * s) {  // play badge on videos
                        ImVec2 m = ia + box * 0.5f;
                        float rad = std::min(g * 0.18f, 22 * s);
                        dl->AddCircleFilled(m, rad, IM_COL32(0, 0, 0, 130));
                        dl->AddTriangleFilled(m + ImVec2(-rad * 0.35f, -rad * 0.5f), m + ImVec2(rad * 0.55f, 0),
                                              m + ImVec2(-rad * 0.35f, rad * 0.5f), IM_COL32(255, 255, 255, 220));
                    }
                }
            }
            if (r.w > 70 && r.h > fs * 2 + 6) {
                dl->PushClipRect(a, b, true);
                Shadowed(dl, a + ImVec2(4, 3), U8(t.Name(r.node)));
                Shadowed(dl, a + ImVec2(4, 3 + fs), FmtBytes(t.nodes[r.node].size));
                dl->PopClipRect();
            }
        }
    }

    // Hit test: rects are in pre-order, so the last one containing the mouse is the deepest.
    int hit = -1;
    bool onHeader = false;
    if (hov) {
        ImVec2 m = ImGui::GetMousePos() - p0;
        for (int i = (int)rects_.size() - 1; i >= 0; i--) {
            const Rect& r = rects_[i];
            if (m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h) {
                hit = i;
                onHeader = r.kind == Folder && r.header > 0 && m.y < r.y + r.header;
                break;
            }
        }
    }
    hovered_ = hit >= 0 && rects_[hit].kind != Rest ? rects_[hit].node : NONE;

    // Duplicate group under the cursor: the item itself, or the duplicate folder it sits in.
    hoverGroup_ = NONE;
    if (dups_)
        for (uint32_t n = hovered_; n != NONE && hoverGroup_ == NONE; n = n == viewRoot_ ? NONE : t.nodes[n].parent)
            hoverGroup_ = GroupOf(n);
    uint32_t litGroup = hoverGroup_ != NONE ? hoverGroup_ : listHoverGroup_;

    // Folder whose name is hovered (its heading here, or its row in the side list): thick border around all its blocks.
    uint32_t litFolder = hit >= 0 && onHeader ? rects_[hit].node
                         : listHovered_ != NONE && t.nodes[listHovered_].isDir ? listHovered_ : NONE;
    if (litFolder != NONE)
        for (const Rect& r : rects_)
            if (r.node == litFolder && r.kind == Folder) {
                float th = std::max(3.0f, 3 * s);
                ImVec2 a = p0 + ImVec2(r.x, r.y), b = a + ImVec2(r.w, r.h), in(th * 0.5f, th * 0.5f);
                dl->AddRect(a + in, b - in, Rgb(theme::kBrightWhite), 0, 0, th);  // inset so the map edge can't clip it
                break;
            }

    // Highlights: selection, hover in the side list, copies of the hovered duplicate
    for (const Rect& r : rects_) {
        if (r.kind == Rest) continue;
        ImVec2 a = p0 + ImVec2(r.x, r.y), b = a + ImVec2(r.w, r.h);
        if (litGroup != NONE && GroupOf(r.node) == litGroup) dl->AddRect(a, b, Rgb(theme::kDup), 0, 0, 3);
        if (IsSelected(r.node)) dl->AddRect(a, b, Rgb(theme::kSelect), 0, 0, 2.5f);
        else if (r.node == listHovered_) dl->AddRect(a, b, Rgb(theme::kBrightWhite), 0, 0, 2);
    }
    if (hit >= 0) {
        const Rect& r = rects_[hit];
        ImVec2 a = p0 + ImVec2(r.x, r.y);
        ImVec2 b = onHeader ? a + ImVec2(r.w, r.header) : a + ImVec2(r.w, r.h);
        dl->AddRect(a, b, Rgb(theme::kBrightWhite, 230), 0, 0, 1.5f);
        if (onHeader && !ImGui::GetIO().KeyCtrl) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    dl->PopClipRect();

    if (hit >= 0) {
        const Rect& r = rects_[hit];
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ImGui::IsMouseDoubleClicked(0)) {
            // Only act if the first click didn't already change the view under the cursor.
            if (!ctrl && !clickZoomed_ && clickNode_ == r.node) Activate(r.node, r.kind);
        } else if (ImGui::IsItemClicked(0)) {
            clickNode_ = r.node;
            clickZoomed_ = onHeader && !ctrl;
            dragFrom_ = r.kind == Rest || clickZoomed_ ? NONE : r.node;  // a heading click zooms instead
            if (r.kind == Rest) {}  // a Rest block stands for many items, not its folder
            else if (ctrl) Select(r.node, true);  // Ctrl+click (headings too) adds to / removes from the selection
            else if (onHeader) Zoom(r.node);
            else Select(r.node, false);
        }
        if (ImGui::IsItemClicked(1)) OpenContext(r.node, r.kind == Rest);
        // Scroll wheel over a video scrubs through it (in the block and the hover preview).
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && showThumbs_ && r.kind == Tile && MediaKind(r.node) == 2 && !(dim && !lit_[r.node])) {
            ImVec2 box = TooltipBox();
            video_.Play(t.Path(r.node), (int)box.x, (int)box.y);
            video_.Scrub(-wheel);  // wheel down = forward
        }
        if (!ImGui::IsPopupOpen("ctx")) Tooltip(r);
    }
    ContextMenu();
}

void App::Tooltip(const Rect& r) {
    Tree& t = *tree_;
    const Node& n = t.nodes[r.node];
    const Node& root = t.nodes[0];
    const Node& cur = t.nodes[viewRoot_];
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    if (r.kind == Rest) {
        ImGui::Text("Smaller items in %s", U8(t.Name(r.node)).c_str());
        ImGui::TextDisabled("Too small to draw individually - zoom in to see them");
    } else {
        bool dimmed = showDups_ && !lit_.empty() && !lit_[r.node];
        if (showThumbs_ && MediaKind(r.node) && !dimmed) Preview(r.node);
        ImGui::TextUnformatted(U8(t.Name(r.node)).c_str());
        if (n.isDir) ImGui::TextDisabled("Folder  ·  %s files", FmtCount(n.files).c_str());
        else ImGui::TextDisabled("%s file", U8(t.exts[n.ext].ext).c_str());
        ImGui::Separator();
        ImGui::Text("%s", FmtBytes(n.size).c_str());
        ImGui::Text("%s of scan  ·  %s of disk", FmtPct(root.size ? (double)n.size / root.size : 0).c_str(),
                    FmtPct(t.diskTotal ? (double)n.size / t.diskTotal : 0).c_str());
        if (viewRoot_ != 0) ImGui::Text("%s of this folder", FmtPct(cur.size ? (double)n.size / cur.size : 0).c_str());
        if (uint32_t g = GroupOf(r.node); g != NONE) {
            const DupGroup& grp = dups_->groups[g];
            ImGui::TextColored(Vec(theme::kDup), "Duplicate: %zu identical copies  ·  %s reclaimable",
                               grp.members.size(), FmtBytes(grp.size * (grp.members.size() - 1)).c_str());
        } else if (hoverGroup_ != NONE) {
            ImGui::TextColored(Vec(theme::kDup), "Inside a duplicated folder (%zu copies)",
                               dups_->groups[hoverGroup_].members.size());
        }
        ImGui::TextDisabled("%s", U8(t.Path(r.node)).c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// Large picture, or the video playing silently on a loop, at the top of the tooltip.
void App::Preview(uint32_t n) {
    ImVec2 box = TooltipBox();
    float maxW = box.x, maxH = box.y;
    std::wstring path = tree_->Path(n);
    if (previewNode_ != n) {
        previewNode_ = n;
        previewSince_ = ImGui::GetTime();
    }
    const Texture* tex = nullptr;
    float progress = -1;
    if (MediaKind(n) == 2) {
        videoWanted_ = true;
        // Short delay so sweeping the mouse across a folder of videos doesn't open every one.
        if (ImGui::GetTime() - previewSince_ > 0.25) video_.Play(path, (int)maxW, (int)maxH);
        if (video_.Path() == path) {
            tex = video_.Frame(ctx_);
            progress = video_.Progress();
        }
        if (!tex) tex = thumbs_->Get(path, 1024, 1e9f);  // poster frame until playback starts
    } else {
        tex = thumbs_->Get(path, 1024, 1e9f);
    }
    if (!tex) {
        ImGui::TextDisabled("Loading preview...");
        ImGui::Separator();
        return;
    }
    float k = std::min({maxW / tex->w, maxH / tex->h, 2.0f});
    ImGui::Image((ImTextureID)(intptr_t)tex->srv, ImVec2(tex->w * k, tex->h * k));
    if (progress >= 0) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float h = 3 * ImGui::GetStyle().FontScaleDpi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(a.x, b.y - h), b, IM_COL32(0, 0, 0, 140));
        dl->AddRectFilled(ImVec2(a.x, b.y - h), ImVec2(a.x + (b.x - a.x) * progress, b.y), Rgb(theme::kBrightYellow));
    }
    ImGui::Separator();
}

// Double-click / "Preview": the picture as large as fits, or the video with pause and seeking.
void App::PreviewModal() {
    if (previewRequest_ != NONE) {
        modalPath_ = tree_->Path(previewRequest_);
        modalKind_ = MediaKind(previewRequest_);
        modalSize_ = tree_->nodes[previewRequest_].size;
        previewRequest_ = NONE;
        if (modalKind_ == 2) video_.SetPaused(false);
        ImGui::OpenPopup("###preview");
    }
    CenterNextModal(0);
    std::string title = U8(modalPath_.substr(modalPath_.find_last_of(L'\\') + 1)) + "###preview";
    bool open = true;
    // NoNav: Space and the arrow keys belong to the player here, not keyboard navigation.
    if (!ImGui::BeginPopupModal(title.c_str(), &open,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        return;
    float s = ImGui::GetStyle().FontScaleDpi;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    bool video = modalKind_ == 2;
    // Largest the picture may be: most of the window, minus the title bar and `rows` rows of controls.
    float rowH = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    auto maxBox = [&](int rows) {
        float chrome = ImGui::GetFrameHeight() + rowH * rows + style.ItemSpacing.y + style.WindowPadding.y * 2;
        return ImVec2(std::floor(vp->WorkSize.x * 0.88f), std::floor(vp->WorkSize.y * 0.9f - chrome));
    };
    int wideRows = video ? 2 : 1;
    ImVec2 box = maxBox(wideRows);

    const Texture* tex = nullptr;
    if (video) {
        videoWanted_ = true;
        video_.Play(modalPath_, (int)box.x, (int)box.y, true);
        video_.SetVolume(volume_);
        tex = video_.Frame(ctx_);
        if (!tex) tex = thumbs_->Get(modalPath_, 1024, 1e9f);  // poster frame until playback starts
    } else {
        tex = big_->Get(modalPath_, 2048, 1);
        if (!tex) tex = thumbs_->Get(modalPath_, 1024, 1e9f);  // smaller copy while the big one loads
    }

    // The dialog takes the picture's shape (16:9 until something has loaded). When that's too narrow
    // for the controls in one row they wrap onto more rows; only very thin pictures get side bars.
    float aspect = tex ? (float)tex->w / tex->h : 16.0f / 9.0f;
    auto fit = [&](ImVec2 b) {
        ImVec2 st = aspect > b.x / b.y ? ImVec2(b.x, std::floor(b.x / aspect)) : ImVec2(std::floor(b.y * aspect), b.y);
        if (!video && tex && tex == big_->Peek(modalPath_)) {  // full-size copy: don't blow small pictures up past 2x
            float k = std::min(1.0f, 2.0f * tex->w / st.x);
            st = ImVec2(std::floor(st.x * k), std::floor(st.y * k));
        }
        return st;
    };
    const char* openLabel = "Open in default app";
    float timeW = ImGui::CalcTextSize("0:00:00 / 0:00:00").x, volW = 90 * s, pauseW = 80 * s;
    float speakerW = ImGui::GetFrameHeight(), sp = style.ItemSpacing.x;
    float openW = ImGui::CalcTextSize(openLabel).x + style.FramePadding.x * 2 + 16 * s, explorerW = 130 * s;
    float deleteW = 90 * s, closeW = 80 * s;
    float buttonsW = openW + explorerW + deleteW + closeW + sp * 3;
    float wideMin = std::max(buttonsW + 220 * s, video ? pauseW + 160 * s + timeW + speakerW + volW + sp * 4 : 0);
    ImVec2 stage = fit(box);
    bool narrow = stage.x < wideMin;
    if (narrow) stage = fit(maxBox(wideRows + (video ? 3 : 1)));
    float width = std::max(stage.x, 340 * s);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("stage", ImVec2(width, stage.y));
    bool stageClicked = ImGui::IsItemClicked(0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = p + ImVec2(std::floor((width - stage.x) * 0.5f), 0);
    if (tex) {
        dl->AddImage((ImTextureID)(intptr_t)tex->srv, a, a + stage);
    } else {
        const char* msg = "Loading...";
        dl->AddRectFilled(a, a + stage, Rgb(theme::kDeep));
        dl->AddText(a + (stage - ImGui::CalcTextSize(msg)) * 0.5f, Rgb(theme::kBrightBlack), msg);
    }
    float x0 = ImGui::GetCursorPosX();

    if (video) {
        bool paused = video_.Paused();
        if (stageClicked || ImGui::IsKeyPressed(ImGuiKey_Space, false)) video_.SetPaused(paused = !paused);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) video_.Scrub(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) video_.Scrub(1);

        // Wide:   [Pause] [seek bar ............] 1:23 / 4:56  [speaker] [volume]
        // Narrow: [seek bar .......................................]
        //         [Pause] 1:23 / 4:56                [speaker] [volume]
        double dur = video_.Duration();
        // While the bar is dragged it shows where the mouse is, not the keyframe being displayed.
        float pos = seekDragging_ ? seekDrag_ : video_.Progress();
        auto seekBar = [&](float w) {
            ImGui::SetNextItemWidth(w);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Vec(theme::kBrightYellow));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Vec(theme::kBrightYellow));
            bool moved = ImGui::SliderFloat("##pos", &pos, 0, 1, "");
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemActive()) {
                seekDragging_ = true;
                seekDrag_ = pos;
                if (moved && dur > 0) video_.SeekTo(pos, false);  // quick keyframe seeks while dragging
            }
            if (ImGui::IsItemDeactivated()) {
                seekDragging_ = false;
                if (dur > 0) video_.SeekTo(seekDrag_, true);  // exact frame where it was let go
            }
            if (ImGui::IsItemHovered() && !seekDragging_)
                ImGui::SetTooltip("Drag, scroll or ←/→ to seek  ·  Space or click the video to pause");
        };
        if (narrow) seekBar(width);
        if (ImGui::Button(paused ? "Play###pp" : "Pause###pp", ImVec2(pauseW, 0))) video_.SetPaused(!paused);
        ImGui::SameLine();
        if (!narrow) {
            seekBar(width - pauseW - timeW - speakerW - volW - sp * 4);
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        std::string time = FmtTime(pos * dur) + " / " + FmtTime(dur);
        ImGui::TextUnformatted(time.c_str());
        ImGui::SameLine(x0 + width - volW - speakerW - sp);
        if (ImGui::Button("##mute", ImVec2(speakerW, 0))) {
            if (volume_ > 0) { mutedVolume_ = volume_; volume_ = 0; }
            else volume_ = mutedVolume_ > 0 ? mutedVolume_ : 0.8f;
        }
        {  // speaker icon, with sound waves or a cross when muted
            ImVec2 c = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f - ImVec2(3 * s, 0);
            float u = speakerW * 0.11f;
            ImU32 col = Rgb(theme::kForeground);
            dl->AddRectFilled(c + ImVec2(-3 * u, -1.2f * u), c + ImVec2(-1.5f * u, 1.2f * u), col);
            dl->AddQuadFilled(c + ImVec2(-1.5f * u, -1.2f * u), c + ImVec2(0.5f * u, -3 * u), c + ImVec2(0.5f * u, 3 * u),
                              c + ImVec2(-1.5f * u, 1.2f * u), col);
            if (volume_ > 0) {
                dl->PathArcTo(c + ImVec2(0.5f * u, 0), 1.8f * u, -0.9f, 0.9f);
                dl->PathStroke(col, 0, 1.4f * s);
                if (volume_ > 0.5f) {
                    dl->PathArcTo(c + ImVec2(0.5f * u, 0), 3.2f * u, -0.9f, 0.9f);
                    dl->PathStroke(col, 0, 1.4f * s);
                }
            } else {
                ImVec2 x = c + ImVec2(3 * u, 0);
                dl->AddLine(x + ImVec2(-u, -u), x + ImVec2(u, u), col, 1.4f * s);
                dl->AddLine(x + ImVec2(-u, u), x + ImVec2(u, -u), col, 1.4f * s);
            }
        }
        bool overVolume = ImGui::IsItemHovered();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(volW);
        char volText[16];  // used as a format string, so the % is doubled
        snprintf(volText, sizeof volText, "%d%%%%", (int)std::lround(volume_ * 100));
        ImGui::SliderFloat("##vol", &volume_, 0, 1, volText);
        overVolume |= ImGui::IsItemHovered();
        if (overVolume) ImGui::SetTooltip("Volume (scroll to change, click the speaker to mute)");

        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && overVolume) volume_ = std::clamp(volume_ + wheel * 0.05f, 0.0f, 1.0f);
        else if (wheel != 0 && ImGui::IsWindowHovered()) video_.Scrub(-wheel);  // wheel down = forward
        video_.SetVolume(volume_);
    }

    // Wide:   size · path ..........  [Open in default app] [Show in Explorer] [Delete...] [Close]
    // Narrow: size · path ....................................
    //         [Open]      [Explorer]      [Delete...]      [Close]
    const char* explorerLabel = "Show in Explorer";
    if (narrow) {
        openLabel = "Open###open";
        explorerLabel = "Explorer###explorer";
        openW = explorerW = deleteW = closeW = std::floor((width - sp * 3) / 4);
        buttonsW = width;
    }
    std::string info = FmtBytes(modalSize_) + "  ·  ";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(info.c_str());
    ImGui::SameLine(0, 0);
    float pathW = width - ImGui::CalcTextSize(info.c_str()).x - (narrow ? 0 : buttonsW + 16 * s);
    std::string shown = EllipsizeLeft(U8(modalPath_), pathW);
    ImGui::TextDisabled("%s", shown.c_str());
    if (shown.size() != U8(modalPath_).size() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", U8(modalPath_).c_str());
    if (!narrow) ImGui::SameLine(x0 + width - buttonsW);
    if (ImGui::Button(openLabel, ImVec2(openW, 0))) {
        if ((INT_PTR)ShellExecuteW(hwnd_, nullptr, modalPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
            ShellExecuteW(hwnd_, L"openas", modalPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        open = false;
    }
    if (narrow && ImGui::IsItemHovered()) ImGui::SetTooltip("Open in default app");
    ImGui::SameLine();
    if (ImGui::Button(explorerLabel, ImVec2(explorerW, 0))) {
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(modalPath_.c_str())) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
        }
    }
    if (narrow && ImGui::IsItemHovered()) ImGui::SetTooltip("Show in Explorer");
    ImGui::SameLine();
    // Closes the preview (which also lets go of the file) and asks in the usual delete dialog.
    PushDangerButton();
    bool del = ImGui::Button("Delete...", ImVec2(deleteW, 0)) || ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    ImGui::PopStyleColor(3);
    if (del) {
        if (uint32_t n = FindPath(modalPath_); n != NONE) RequestDelete({n});
        open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(closeW, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
    if (!open) {
        seekDragging_ = false;
        if (video) video_.SetPaused(false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::ContextMenu() {
    if (!ImGui::BeginPopup("ctx")) return;
    Tree& t = *tree_;
    const Node& n = t.nodes[ctxNode_];
    if (ctxRest_) {
        // A block of many small items: only offer to look closer.
        ImGui::TextDisabled("Small items in %s", U8(t.Name(ctxNode_)).c_str());
        ImGui::Separator();
        if (ctxNode_ != viewRoot_ && ImGui::MenuItem("Zoom in to see them")) Zoom(ctxNode_);
        if (ImGui::MenuItem("Open folder")) OpenItem(ctxNode_);
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("%s", U8(t.Name(ctxNode_)).c_str());
    ImGui::Separator();
    if (MediaKind(ctxNode_)) {
        if (ImGui::MenuItem("Preview", "Double-click")) RequestPreview(ctxNode_);
        if (ImGui::MenuItem("Open in default app")) OpenItem(ctxNode_);
    } else if (ImGui::MenuItem("Open", "Double-click")) {
        OpenItem(ctxNode_);
    }
    if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(ctxNode_);
    uint32_t zoomTo = (n.isDir && n.count) ? ctxNode_ : n.parent;
    if (zoomTo != NONE && zoomTo != viewRoot_) {
        std::string label = "Zoom into " + U8(t.Name(zoomTo)) + "###zoom";
        if (ImGui::MenuItem(label.c_str())) Zoom(zoomTo);
    }
    if (viewRoot_ != 0 && ImGui::MenuItem("Zoom out")) Up();
    ImGui::Separator();
    if (sel_.size() > 1 && IsSelected(ctxNode_)) {
        if (ImGui::MenuItem("Copy paths")) {
            std::string all;
            for (uint32_t n : sel_) all += U8(t.Path(n)) + "\r\n";
            ImGui::SetClipboardText(all.c_str());
        }
    } else if (ImGui::MenuItem("Copy path")) {
        ImGui::SetClipboardText(U8(t.Path(ctxNode_)).c_str());
    }
    if (ctxNode_ != 0) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, Vec(theme::kError));
        bool many = sel_.size() > 1 && IsSelected(ctxNode_);
        std::string label = many ? "Delete " + FmtCount(sel_.size()) + " selected items...###del" : "Delete...###del";
        if (ImGui::MenuItem(label.c_str(), "Del")) RequestDelete(many ? sel_ : std::vector<uint32_t>{ctxNode_});
        ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
}

void App::DrawSide() {
    Tree& t = *tree_;
    float s = ImGui::GetStyle().FontScaleDpi;
    const Node& cur = t.nodes[viewRoot_];
    const Node& root = t.nodes[0];
    float swatch = ImGui::GetFontSize() * 0.7f;
    auto drawSwatch = [&](Color c) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float off = (ImGui::GetTextLineHeight() - swatch) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(0, off), p + ImVec2(swatch, off + swatch),
                                                  Rgb(c), 2 * s);
        ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
    };

    listHovered_ = NONE;
    float h = ImGui::GetContentRegionAvail().y;
    ImGui::SeparatorText("Largest in this folder");
    if (ImGui::BeginChild("top", ImVec2(0, h * 0.55f), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("toptbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed);
            uint32_t shown = std::min<uint32_t>(cur.count, 500);
            for (uint32_t c = cur.first; c < cur.first + shown; c++) {
                const Node& n = t.nodes[c];
                ImGui::PushID((int)c);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                drawSwatch(NodeColor(c));
                std::string name = U8(t.Name(c)) + (n.isDir ? "\\" : "");
                if (ImGui::Selectable(name.c_str(), IsSelected(c),
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    Select(c, ImGui::GetIO().KeyCtrl);
                    if (ImGui::IsMouseDoubleClicked(0)) Activate(c, Tile);
                }
                if (ImGui::IsItemHovered()) listHovered_ = c;
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8 * s)) dragRequest_ = c;
                if (ImGui::IsItemClicked(1)) OpenContext(c);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s  %5.1f%%", FmtBytes(n.size).c_str(), cur.size ? 100.0 * n.size / cur.size : 0.0);
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (cur.count > shown) ImGui::TextDisabled("+ %s more", FmtCount(cur.count - shown).c_str());
        }
        ContextMenu();
    }
    ImGui::EndChild();

    if (!ImGui::BeginTabBar("lower")) return;
    std::string dupTab = dups_ ? "Duplicates (" + FmtCount(std::count_if(dups_->groups.begin(), dups_->groups.end(),
                                                                          [](const DupGroup& g) { return g.members.size() >= 2; })) +
                                     ")###dups"
                               : "Duplicates###dups";
    bool showExts = ImGui::BeginTabItem("File types");
    if (showExts) ImGui::EndTabItem();
    if (ImGui::BeginTabItem(dupTab.c_str(), nullptr, dupJustFound_ ? ImGuiTabItemFlags_SetSelected : 0)) {
        if (ImGui::BeginChild("dups", ImVec2(0, 0), ImGuiChildFlags_Borders)) DrawDupList();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    dupJustFound_ = false;
    ImGui::EndTabBar();
    if (!showExts) return;
    if (ImGui::BeginChild("exts", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("exttbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("ext", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed);
            for (size_t i = 0; i < extOrder_.size() && i < 200; i++) {
                const ExtInfo& e = t.exts[extOrder_[i]];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                drawSwatch(extColor_[extOrder_[i]]);
                ImGui::Text("%s", U8(e.ext).c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", FmtCount(e.count).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s  %5.1f%%", FmtBytes(e.bytes).c_str(), root.size ? 100.0 * e.bytes / root.size : 0.0);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}
