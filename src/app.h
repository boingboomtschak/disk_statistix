#pragma once
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "dupes.h"
#include "imgui.h"
#include "media.h"
#include "scanner.h"

class App {
public:
    App(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx, const std::wstring& initialScan);
    void Frame();

private:
    enum class Mode { SelectDisk, Scanning, View };
    enum RectKind : uint8_t { Folder, Tile, Rest };  // Rest = tiny siblings lumped together

    struct Drive {
        std::wstring root;
        std::string label, type;
        uint64_t total = 0, free = 0;
    };
    struct Rect {
        float x, y, w, h;
        uint32_t node;  // for Rest: the folder whose small children it holds
        uint16_t depth;
        RectKind kind;
        float header;   // height of the folder's title strip, 0 if none
    };
    using Color = uint32_t;  // 0xRRGGBB

    // UI sections
    void DiskModal();
    void ScanModal();
    void RescanModal();
    void DeleteModal();
    void ConfirmPermanentModal(bool& close);
    void ErrorsModal();
    void DupOptionsModal();
    void DupScanModal();
    void DrawDupList();
    void Blank();
    static void DrawLogo(ImDrawList* dl, ImVec2 center, float size, float alpha);
    void RecomputeLit();
    uint32_t GroupOf(uint32_t n) const { return dups_ && n != NONE ? dups_->groupOf[n] : NONE; }
    void DrawView();
    void DrawMap();
    void DrawSide();
    void ContextMenu();
    void Tooltip(const Rect& r);
    void Preview(uint32_t n);
    void PreviewModal();
    void RequestPreview(uint32_t n);  // opens the preview dialog if n is a picture or video
    ImVec2 TooltipBox() const;
    int MediaKind(uint32_t n) const;  // 0 none, 1 image, 2 video
    bool IsSelected(uint32_t n) const { return std::find(sel_.begin(), sel_.end(), n) != sel_.end(); }
    void Select(uint32_t n, bool toggle);

    // Treemap
    void Relayout(ImVec2 size);
    void LayoutNode(uint32_t n, float x, float y, float w, float h, int depth);
    void Squarify(uint32_t dir, float x, float y, float w, float h, int depth);
    Color NodeColor(uint32_t n) const;

    // Actions
    void RefreshDrives();
    void StartScan(const std::wstring& path);
    void OnTreeReady();
    void Zoom(uint32_t n);
    void Up();
    void Navigate(uint32_t n);
    void HistoryStep(int dir);  // -1 back, +1 forward
    void Activate(uint32_t n, RectKind kind);  // double-click behaviour
    void OpenItem(uint32_t n);
    void ShowInExplorer(uint32_t n);
    void OpenContext(uint32_t n, bool rest = false);
    std::wstring BrowseFolder();
    void RequestDelete(std::vector<uint32_t> nodes);
    void Delete(const std::vector<uint32_t>& nodes, bool permanent);
    uint32_t FindPath(const std::wstring& path) const;
    void RemoveFromTree(uint32_t n);
    size_t RemoveMissing(const std::vector<std::wstring>& paths, uint64_t& freed);
    void DragOut(uint32_t n);
    void SwapSiblings(uint32_t a, uint32_t b);
    void SetStatus(std::string msg, bool error);

    HWND hwnd_;
    ID3D11DeviceContext* ctx_;
    Mode mode_ = Mode::SelectDisk;
    std::vector<Drive> drives_;
    int driveSel_ = -1;
    std::wstring scanTarget_;
    Scanner scanner_;
    std::unique_ptr<Tree> tree_;
    std::vector<Color> extColor_;
    std::vector<uint32_t> extOrder_;

    std::vector<uint32_t> sel_;  // ctrl+click adds; the last one is the "current" item
    uint32_t viewRoot_ = 0, hovered_ = NONE, listHovered_ = NONE, ctxNode_ = NONE;
    std::vector<std::wstring> history_;  // folders visited, as paths so deletes can't scramble them
    size_t histPos_ = 0;
    uint32_t clickNode_ = NONE;
    uint32_t dragFrom_ = NONE, dragRequest_ = NONE;  // drag-out: item pressed on the map / item to drag now
    bool clickZoomed_ = false;
    bool ctxRest_ = false;

    // Modals opened from nested windows are requested here and opened at the top level.
    bool rescanRequest_ = false, errorsRequest_ = false;
    std::vector<uint32_t> deleteRequest_, deleteNodes_;
    // Duplicates
    DupFinder dupFinder_;
    std::unique_ptr<DupResult> dups_;
    std::vector<uint8_t> lit_;      // node is a duplicate or inside a duplicate folder
    std::vector<uint8_t> dupOpen_;  // expanded groups in the side list
    bool showDups_ = true, dupFolders_ = true, dupOptionsRequest_ = false, dupScanning_ = false, dupJustFound_ = false;
    int dupMinIdx_ = 3;
    uint32_t hoverGroup_ = NONE, listHoverGroup_ = NONE;

    // Thumbnails and hover previews
    std::unique_ptr<ThumbCache> thumbs_;
    std::unique_ptr<ThumbCache> big_;  // full-size pictures for the preview dialog, kept apart from the tiles
    VideoPlayer video_;
    std::vector<uint8_t> extMedia_;  // per extension: 0 none, 1 image, 2 video
    bool showThumbs_ = false, videoWanted_ = false;
    uint32_t previewNode_ = NONE, previewRequest_ = NONE;
    double previewSince_ = 0;
    std::wstring modalPath_;  // preview dialog's file; a path, so it survives tree edits
    int modalKind_ = 0;
    uint64_t modalSize_ = 0;
    float volume_ = 0.8f, mutedVolume_ = 0;  // preview dialog sound
    bool seekDragging_ = false;
    float seekDrag_ = 0;

    std::string status_;
    bool statusError_ = false;
    double statusUntil_ = 0;

    std::vector<Rect> rects_;
    uint32_t layoutRoot_ = NONE;
    ImVec2 layoutSize_{0, 0};
    float headerH_ = 0;
};
