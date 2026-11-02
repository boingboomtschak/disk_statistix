#pragma once
// Thumbnails (via the Explorer thumbnail cache) and silent video preview (via Media Foundation),
// decoded on background threads and uploaded to D3D11 textures on the UI thread.
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};

// ImGui draws textures after the frame is built, so a texture released mid-frame (or in EndFrame)
// may still be in that frame's draw list. Released views wait here for two frames first.
class Graveyard {
public:
    ~Graveyard();
    void Add(ID3D11ShaderResourceView* srv);
    void EndFrame();

private:
    std::vector<std::pair<ID3D11ShaderResourceView*, uint64_t>> items_;
    uint64_t frame_ = 0;
};

class ThumbCache {
public:
    ThumbCache(ID3D11Device* device, int threads);
    ~ThumbCache();

    // Best texture loaded so far for `path` (may be smaller than asked, or null), and queues a
    // load at `size` px if nothing that big is loaded yet. Higher priority loads first.
    const Texture* Get(const std::wstring& path, int size, float priority);
    const Texture* Peek(const std::wstring& path) const;  // loaded texture, without queueing anything
    void EndFrame();  // submit this frame's requests, upload finished thumbnails, evict old ones
    void Clear();

private:
    struct Job { std::wstring path; int size; float priority; };
    struct Result { std::wstring path; int size, w, h; std::vector<uint32_t> px; };
    struct Entry {
        Texture tex;
        int loaded = 0;   // size bucket of tex, 0 = none
        bool failed = false;
        uint64_t lastUsed = 0;
    };
    void Worker();
    static bool Load(const std::wstring& path, int size, Result& out);

    ID3D11Device* device_;
    Graveyard graveyard_;
    std::unordered_map<std::wstring, Entry> entries_;
    std::vector<Job> wanted_;
    uint64_t frame_ = 0, bytes_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Job> queue_;  // sorted, highest priority last
    std::deque<Result> done_;
    std::unordered_set<std::wstring> inFlight_;  // being decoded now, so not queued twice
    std::vector<std::thread> threads_;
    bool quit_ = false;
};

class VideoPlayer {
public:
    explicit VideoPlayer(ID3D11Device* device) : device_(device) {}
    ~VideoPlayer();
    // Loops; sound only if `audio`. Playing the same file at a different size, or with sound
    // switched, restarts it where it was.
    void Play(const std::wstring& path, int maxW, int maxH, bool audio = false);
    void Stop();
    const std::wstring& Path() const { return path_; }
    const Texture* Frame(ID3D11DeviceContext* ctx);  // latest decoded frame, null until the first one
    float Progress() const { return progress_; }
    double Duration() const { return duration_; }  // seconds, 0 until known
    void Scrub(float amount);   // jump by amount * 2% of the length (negative = back)
    // exact = false shows the nearest keyframe at or before the spot, which is much quicker (for dragging).
    void SeekTo(float fraction, bool exact = true);
    void SetVolume(float v) { volume_ = v; }  // 0..1
    void SetPaused(bool p) { paused_ = p; }
    bool Paused() const { return paused_; }
    void EndFrame() { graveyard_.EndFrame(); }

private:
    void Run(std::wstring path, int maxW, int maxH, bool audio);

    ID3D11Device* device_;
    std::wstring path_;
    int maxW_ = 0, maxH_ = 0;
    bool audio_ = false;
    std::thread thread_;
    std::atomic<bool> stop_{false}, paused_{false};
    std::atomic<float> progress_{0}, volume_{1};
    std::atomic<double> duration_{0};

    std::mutex mutex_;
    float scrub_ = 0;    // pending relative seek, in fractions of the length
    float seekTo_ = -1;  // pending absolute seek, fraction
    bool seekExact_ = true;
    std::vector<uint32_t> frame_;
    int fw_ = 0, fh_ = 0;
    bool fresh_ = false;

    ID3D11Texture2D* tex_ = nullptr;
    Texture view_;
    Graveyard graveyard_;
};
