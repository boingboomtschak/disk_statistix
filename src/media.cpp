#include "media.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <xaudio2.h>

#include <algorithm>
#include <chrono>
#include <utility>

template <class T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

Graveyard::~Graveyard() {
    for (auto& [srv, frame] : items_) srv->Release();
}

void Graveyard::Add(ID3D11ShaderResourceView* srv) {
    if (srv) items_.push_back({srv, frame_});
}

// Call once per frame, before ImGui::Render. A view added during frame N (or in its EndFrame) is
// released in the EndFrame of frame N+2, after both frames that could have drawn it have rendered.
void Graveyard::EndFrame() {
    frame_++;
    std::erase_if(items_, [&](auto& it) {
        if (it.second + 2 > frame_) return false;
        it.first->Release();
        return true;
    });
}

static Texture MakeTexture(ID3D11Device* dev, int w, int h, const uint32_t* px) {
    Texture out;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // GDI / Media Foundation byte order, no swizzle needed
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px, (UINT)w * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, &init, &tex))) {
        if (SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &out.srv))) { out.w = w; out.h = h; }
        tex->Release();
    }
    return out;
}

// ---------------------------------------------------------------- thumbnails

static constexpr uint64_t kBudget = 256ull << 20;  // GPU bytes of thumbnails kept around

static int Bucket(int size) {
    return size <= 64 ? 64 : size <= 128 ? 128 : size <= 256 ? 256 : size <= 1024 ? 1024 : 2048;
}

ThumbCache::ThumbCache(ID3D11Device* device, int threads) : device_(device) {
    for (int i = 0; i < threads; i++) threads_.emplace_back(&ThumbCache::Worker, this);
}

ThumbCache::~ThumbCache() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    Clear();
}

void ThumbCache::Clear() {
    for (auto& [path, e] : entries_) graveyard_.Add(std::exchange(e.tex.srv, nullptr));
    entries_.clear();
    wanted_.clear();
    bytes_ = 0;
    std::lock_guard lock(mutex_);
    queue_.clear();
    done_.clear();
}

const Texture* ThumbCache::Get(const std::wstring& path, int size, float priority) {
    int b = Bucket(size);
    Entry& e = entries_[path];
    e.lastUsed = frame_;
    if (!e.failed && e.loaded < b) wanted_.push_back({path, b, priority});
    return e.loaded ? &e.tex : nullptr;
}

const Texture* ThumbCache::Peek(const std::wstring& path) const {
    auto it = entries_.find(path);
    return it != entries_.end() && it->second.loaded ? &it->second.tex : nullptr;
}

void ThumbCache::EndFrame() {
    frame_++;
    graveyard_.EndFrame();
    {
        // Replace the queue with what's on screen now; anything scrolled away is dropped.
        std::lock_guard lock(mutex_);
        queue_.clear();
        for (Job& j : wanted_)
            if (!inFlight_.count(j.path)) queue_.push_back(std::move(j));
        std::sort(queue_.begin(), queue_.end(), [](const Job& a, const Job& b) { return a.priority < b.priority; });
    }
    wanted_.clear();
    cv_.notify_all();

    std::vector<Result> got;
    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < 32 && !done_.empty(); i++) {
            got.push_back(std::move(done_.front()));
            done_.pop_front();
        }
    }
    for (Result& r : got) {
        auto it = entries_.find(r.path);
        if (it == entries_.end()) continue;
        Entry& e = it->second;
        if (r.px.empty()) {
            if (!e.loaded) e.failed = true;  // no thumbnail (codec missing, corrupt, offline file...)
            continue;
        }
        if (r.size <= e.loaded) continue;
        Texture t = MakeTexture(device_, r.w, r.h, r.px.data());
        if (!t.srv) continue;
        if (e.tex.srv) bytes_ -= (uint64_t)e.tex.w * e.tex.h * 4;
        graveyard_.Add(std::exchange(e.tex.srv, nullptr));
        e.tex = t;
        e.loaded = r.size;
        bytes_ += (uint64_t)t.w * t.h * 4;
    }

    if (bytes_ > kBudget) {
        // Least recently drawn first; never what's on screen this frame or the last.
        std::vector<std::pair<uint64_t, Entry*>> old;
        for (auto& [path, e] : entries_)
            if (e.loaded && e.lastUsed + 2 < frame_) old.push_back({e.lastUsed, &e});
        std::sort(old.begin(), old.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [used, e] : old) {
            if (bytes_ <= kBudget * 3 / 4) break;
            bytes_ -= (uint64_t)e->tex.w * e->tex.h * 4;
            graveyard_.Add(std::exchange(e->tex.srv, nullptr));
            e->tex = {};
            e->loaded = 0;
        }
    }
    if (entries_.size() > 50000)
        std::erase_if(entries_, [&](auto& kv) { return !kv.second.loaded && kv.second.lastUsed + 2 < frame_; });
}

void ThumbCache::Worker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            job = std::move(queue_.back());
            queue_.pop_back();
            inFlight_.insert(job.path);
        }
        Result r{job.path, job.size, 0, 0, {}};
        if (!Load(job.path, job.size, r)) r.px.clear();
        std::lock_guard lock(mutex_);
        inFlight_.erase(job.path);
        done_.push_back(std::move(r));
    }
    CoUninitialize();
}

// Ask the shell for the same thumbnail Explorer shows: handles every format with a codec
// installed (HEIC, RAW, videos...), honours EXIF rotation, and uses the thumbnail cache.
bool ThumbCache::Load(const std::wstring& path, int size, Result& out) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP bmp = nullptr;
    HRESULT hr = factory->GetImage(SIZE{size, size}, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bmp);
    factory->Release();
    if (FAILED(hr) || !bmp) return false;

    BITMAP bm{};
    GetObjectW(bmp, sizeof bm, &bm);
    int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    bool ok = w > 0 && h > 0;
    if (ok) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.px.resize((size_t)w * h);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bmp, 0, h, out.px.data(), &bi, DIB_RGB_COLORS) == h;
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(bmp);
    if (!ok) return false;

    // Opaque formats come back with alpha 0; transparent ones are premultiplied.
    bool hasAlpha = std::any_of(out.px.begin(), out.px.end(), [](uint32_t p) { return p >> 24; });
    for (uint32_t& p : out.px) {
        uint32_t a = p >> 24;
        if (!hasAlpha) p |= 0xFF000000u;
        else if (a && a < 255) {
            uint32_t r = std::min(255u, ((p >> 16) & 255) * 255 / a), g = std::min(255u, ((p >> 8) & 255) * 255 / a),
                     b = std::min(255u, (p & 255) * 255 / a);
            p = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    out.w = w;
    out.h = h;
    return true;
}

// ---------------------------------------------------------------- video

VideoPlayer::~VideoPlayer() { Stop(); }

void VideoPlayer::Play(const std::wstring& path, int maxW, int maxH, bool audio) {
    bool same = path == path_ && thread_.joinable();
    if (same && maxW == maxW_ && maxH == maxH_ && audio == audio_) return;
    float resume = same ? progress_.load() : -1;
    bool paused = same && paused_;
    Stop();
    path_ = path;
    maxW_ = maxW;
    maxH_ = maxH;
    audio_ = audio;
    stop_ = false;
    paused_ = paused;
    progress_ = std::max(resume, 0.0f);
    seekTo_ = resume > 0 ? resume : -1;
    seekExact_ = true;
    thread_ = std::thread(&VideoPlayer::Run, this, path, maxW, maxH, audio);
}

void VideoPlayer::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    path_.clear();
    paused_ = false;
    duration_ = 0;
    std::lock_guard lock(mutex_);
    scrub_ = 0;
    seekTo_ = -1;
    fresh_ = false;
    graveyard_.Add(std::exchange(view_.srv, nullptr));
    SafeRelease(tex_);
    view_ = {};
}

const Texture* VideoPlayer::Frame(ID3D11DeviceContext* ctx) {
    std::lock_guard lock(mutex_);
    if (fresh_) {
        fresh_ = false;
        if (!tex_ || view_.w != fw_ || view_.h != fh_) {
            graveyard_.Add(std::exchange(view_.srv, nullptr));
            SafeRelease(tex_);
            view_ = {};
            D3D11_TEXTURE2D_DESC d{};
            d.Width = fw_;
            d.Height = fh_;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (SUCCEEDED(device_->CreateTexture2D(&d, nullptr, &tex_)) &&
                SUCCEEDED(device_->CreateShaderResourceView(tex_, nullptr, &view_.srv))) {
                view_.w = fw_;
                view_.h = fh_;
            }
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (tex_ && SUCCEEDED(ctx->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < fh_; y++)
                memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, frame_.data() + (size_t)y * fw_, (size_t)fw_ * 4);
            ctx->Unmap(tex_, 0);
        }
    }
    return view_.srv ? &view_ : nullptr;
}

void VideoPlayer::Scrub(float amount) {
    std::lock_guard lock(mutex_);
    scrub_ += amount * 0.02f;
}

void VideoPlayer::SeekTo(float fraction, bool exact) {
    std::lock_guard lock(mutex_);
    seekTo_ = std::clamp(fraction, 0.0f, 1.0f);
    seekExact_ = exact;
    scrub_ = 0;
}

static HRESULT SetPosition(IMFSourceReader* reader, LONGLONG to) {
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = to;
    return reader->SetCurrentPosition(GUID_NULL, pos);
}

// The sound track, decoded by a second source reader and played through XAudio2. While it plays,
// its position is the clock the video follows, so the two stay in sync.
class AudioOut {
public:
    ~AudioOut() { Close(); }

    bool Open(const std::wstring& path) {
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader_))) return false;
        reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) return Fail();
        IMFMediaType* want = nullptr;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        HRESULT hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want);
        if (FAILED(hr)) {
            want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want);
        }
        want->Release();
        if (FAILED(hr)) return Fail();

        IMFMediaType* cur = nullptr;
        WAVEFORMATEX* wfx = nullptr;
        UINT32 size = 0;
        if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) return Fail();
        hr = MFCreateWaveFormatExFromMFMediaType(cur, &wfx, &size);
        cur->Release();
        if (FAILED(hr)) return Fail();
        rate_ = wfx->nSamplesPerSec;
        frameBytes_ = wfx->nBlockAlign;
        hr = XAudio2Create(&xa_, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr)) hr = xa_->CreateMasteringVoice(&master_);  // fails without an audio device
        if (SUCCEEDED(hr)) hr = xa_->CreateSourceVoice(&voice_, wfx);
        CoTaskMemFree(wfx);
        if (FAILED(hr) || !rate_ || !frameBytes_) return Fail();
        voice_->Start();
        return true;
    }

    void Close() {
        if (voice_) voice_->DestroyVoice();  // blocks until the audio thread lets go of our buffers
        if (master_) master_->DestroyVoice();
        voice_ = nullptr;
        master_ = nullptr;
        SafeRelease(xa_);
        SafeRelease(reader_);
        bufs_.clear();
    }

    bool Active() const { return voice_ != nullptr; }

    // Drop what's queued and continue from `to`.
    void Seek(LONGLONG to) {
        if (!voice_) return;
        voice_->Stop();
        voice_->FlushSourceBuffers();
        XAUDIO2_VOICE_STATE st;
        for (int i = 0; i < 100; i++) {  // the flush completes on the audio thread
            voice_->GetState(&st, 0);
            if (!st.BuffersQueued) break;
            Sleep(1);
        }
        bufs_.clear();
        submitted_ = st.SamplesPlayed;
        base_ = -1;
        skipUntil_ = to;
        eos_ = false;
        SetPosition(reader_, to);
        if (!paused_) voice_->Start();
    }

    // Keep about 300 ms decoded and queued.
    void Fill() {
        if (!voice_ || eos_) return;
        XAUDIO2_VOICE_STATE st;
        voice_->GetState(&st, 0);
        while (bufs_.size() > st.BuffersQueued) bufs_.pop_front();  // played: safe to free
        UINT64 queued = submitted_ - st.SamplesPlayed;
        while (queued < rate_ * 3 / 10) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            if (FAILED(reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &sample)) ||
                (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                SafeRelease(sample);
                eos_ = true;
                return;
            }
            if (!sample) continue;
            IMFMediaBuffer* mb = nullptr;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mb)) && SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
                DWORD skip = 0;
                if (skipUntil_ >= 0) {  // seeks land a little early; trim to the exact spot
                    if (ts < skipUntil_)
                        skip = (DWORD)std::min<LONGLONG>(len, (skipUntil_ - ts) * rate_ / 10000000 * frameBytes_);
                    if (skip < len) {
                        ts += (LONGLONG)(skip / frameBytes_) * 10000000 / rate_;
                        skipUntil_ = -1;
                    }
                }
                len -= len % frameBytes_;
                if (skip < len) {
                    if (base_ < 0) {
                        base_ = ts;
                        playedAtBase_ = submitted_;
                    }
                    bufs_.emplace_back(data + skip, data + len);
                    XAUDIO2_BUFFER b{};
                    b.AudioBytes = len - skip;
                    b.pAudioData = bufs_.back().data();
                    if (SUCCEEDED(voice_->SubmitSourceBuffer(&b))) {
                        UINT64 frames = (len - skip) / frameBytes_;
                        submitted_ += frames;
                        queued += frames;
                    } else {
                        bufs_.pop_back();
                    }
                }
                mb->Unlock();
            }
            SafeRelease(mb);
            sample->Release();
        }
    }

    // Position of what's coming out of the speakers now; false until it's known.
    bool Clock(LONGLONG& now) {
        if (!voice_ || base_ < 0) return false;
        XAUDIO2_VOICE_STATE st;
        voice_->GetState(&st, 0);
        now = base_ + (LONGLONG)((st.SamplesPlayed - playedAtBase_) * 10000000 / rate_);
        return true;
    }

    // Sound track ended (or is shorter than the video) and everything queued has played.
    bool Drained() {
        if (!voice_ || !eos_) return false;
        XAUDIO2_VOICE_STATE st;
        voice_->GetState(&st, 0);
        return st.BuffersQueued == 0;
    }

    void SetPaused(bool p) {
        if (!voice_ || p == paused_) return;
        paused_ = p;
        if (p) voice_->Stop();
        else voice_->Start();
    }

    void SetVolume(float v) {
        if (voice_ && v != volume_) voice_->SetVolume(v * v);  // squared: closer to how loudness is heard
        volume_ = v;
    }

private:
    bool Fail() {
        Close();
        return false;
    }

    IMFSourceReader* reader_ = nullptr;
    IXAudio2* xa_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    UINT32 rate_ = 0, frameBytes_ = 0;
    std::deque<std::vector<BYTE>> bufs_;  // submitted buffers; XAudio2 reads them in place
    UINT64 submitted_ = 0, playedAtBase_ = 0;
    LONGLONG base_ = -1, skipUntil_ = -1;
    bool eos_ = false, paused_ = false;
    float volume_ = -1;
};

void VideoPlayer::Run(std::wstring path, int maxW, int maxH, bool withAudio) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFSourceReader* reader = nullptr;
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // decode + convert + scale to RGB32
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attr, &reader);
    attr->Release();

    UINT32 w = 0, h = 0;
    LONG stride = 0;
    auto readSize = [&] {
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
            stride = (LONG)MFGetAttributeUINT32(cur, MF_MT_DEFAULT_STRIDE, w * 4);
            cur->Release();
        }
    };
    if (SUCCEEDED(hr)) {
        // Video only here; sound, if wanted, comes from its own reader (AudioOut).
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    }
    if (SUCCEEDED(hr)) {
        UINT32 nw = 0, nh = 0, parN = 1, parD = 1;
        IMFMediaType* native = nullptr;
        if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
            MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &nw, &nh);
            MFGetAttributeRatio(native, MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD);
            native->Release();
        }
        IMFMediaType* out = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (nw && nh && parD) {
            // Scale down to the preview box while decoding, so big videos stay cheap.
            double dispW = (double)nw * parN / parD;
            double k = std::min({1.0, maxW / dispW, (double)maxH / nh});
            UINT32 tw = std::max<UINT32>(2, (UINT32)(dispW * k) & ~1u), th = std::max<UINT32>(2, (UINT32)(nh * k) & ~1u);
            MFSetAttributeSize(out, MF_MT_FRAME_SIZE, tw, th);
            MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        }
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
        if (FAILED(hr)) {  // scaling not supported: take native size
            out->DeleteItem(MF_MT_FRAME_SIZE);
            out->DeleteItem(MF_MT_PIXEL_ASPECT_RATIO);
            hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
        }
        out->Release();
        readSize();
    }

    LONGLONG duration = 0;
    if (SUCCEEDED(hr)) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)))
            duration = (LONGLONG)v.uhVal.QuadPart;
        PropVariantClear(&v);
        duration_ = duration / 1e7;
    }
    AudioOut audio;
    if (SUCCEEDED(hr) && withAudio) audio.Open(path);

    // Playback clock in 100 ns units: the sound's position while it plays, otherwise wall time
    // counted from the frame shown when it (re)started.
    using clock = std::chrono::steady_clock;
    clock::time_point wallStart;
    LONGLONG wallBase = -1;
    auto audioClocked = [&] { return audio.Active() && !audio.Drained(); };
    auto now = [&](LONGLONG& t) {
        if (audioClocked()) return audio.Clock(t);
        if (wallBase < 0) return false;
        t = wallBase + std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - wallStart).count() / 100;
        return true;
    };

    LONGLONG cur = 0, skipUntil = -1;
    bool showOne = false;  // a seek's frame is due even though playback is paused or not yet clocked
    bool seeking = false;  // a seek hasn't shown its frame yet; newer seeks wait for it
    bool wasPaused = false;
    std::vector<uint32_t> buf;
    while (SUCCEEDED(hr) && !stop_ && w && h) {
        audio.SetVolume(volume_);
        if (paused_ != wasPaused) {
            wasPaused = paused_;
            audio.SetPaused(wasPaused);
            wallBase = -1;  // restart wall pacing from the current frame
        }
        if (!seeking) {
            float scrub, seekTo;
            bool exact;
            {
                std::lock_guard lock(mutex_);
                scrub = scrub_;
                seekTo = seekTo_;
                exact = seekExact_;
                scrub_ = 0;
                seekTo_ = -1;
            }
            if ((seekTo >= 0 || scrub != 0) && duration > 0) {
                LONGLONG target = seekTo >= 0 ? (LONGLONG)(seekTo * duration) : cur + (LONGLONG)(scrub * duration);
                target = std::clamp<LONGLONG>(target, 0, duration - duration / 200);
                hr = SetPosition(reader, target);
                audio.Seek(target);
                // Seeks land on the keyframe before the target. An exact seek decodes up to the target
                // without showing those frames; a quick one (dragging the seek bar) shows the keyframe.
                cur = target;
                skipUntil = exact ? target : -1;
                wallBase = -1;
                showOne = seeking = true;
                progress_ = (float)((double)target / duration);
                continue;
            }
        }
        if (paused_ && !showOne) {
            Sleep(10);
            continue;
        }

        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // loop
            SafeRelease(sample);
            hr = SetPosition(reader, 0);
            audio.Seek(0);
            wallBase = skipUntil = -1;
            cur = 0;
            seeking = false;
            continue;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) readSize();
        if (!sample) continue;
        if (skipUntil >= 0) {
            if (ts + 500000 < skipUntil) {  // 50 ms slack: frame times rarely hit the target exactly
                audio.Fill();
                sample->Release();
                continue;
            }
            skipUntil = -1;
        }

        // Wait for the frame's time to come up on the clock.
        bool drop = false;
        if (!showOne) {
            auto waitStart = clock::now();
            for (;;) {
                audio.Fill();
                LONGLONG t;
                if (stop_ || paused_) break;
                if (!now(t)) {
                    if (!audioClocked()) {  // wall clock starts at this frame
                        wallBase = ts;
                        wallStart = clock::now();
                        break;
                    }
                    if (clock::now() - waitStart > std::chrono::seconds(2)) audio.Close();  // sound never started
                    Sleep(2);
                    continue;
                }
                if (t >= ts) {
                    if (t - ts > 5000000) {  // fell over 0.5 s behind (slow decode)
                        if (audioClocked()) drop = true;  // the sound won't wait: skip frames to catch up
                        else wallBase = -1;               // silent: just carry on from here
                    }
                    break;
                }
                Sleep(2);
            }
        }

        IMFMediaBuffer* mb = nullptr;
        if (!drop && SUCCEEDED(sample->ConvertToContiguousBuffer(&mb))) {
            buf.resize((size_t)w * h);
            IMF2DBuffer* b2 = nullptr;
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            bool locked2d = SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch));
            BYTE* data = nullptr;
            DWORD len = 0;
            if (!locked2d && SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
                pitch = stride;
                scan0 = pitch < 0 ? data + (size_t)(-pitch) * (h - 1) : data;
            }
            if (scan0) {
                for (UINT32 y = 0; y < h; y++) {
                    const uint32_t* row = (const uint32_t*)(scan0 + (ptrdiff_t)pitch * (LONG)y);
                    for (UINT32 x = 0; x < w; x++) buf[(size_t)y * w + x] = row[x] | 0xFF000000u;  // X8 -> opaque
                }
                std::lock_guard lock(mutex_);
                frame_.swap(buf);
                fw_ = (int)w;
                fh_ = (int)h;
                fresh_ = true;
            }
            if (locked2d) b2->Unlock2D();
            else if (data) mb->Unlock();
            SafeRelease(b2);
            mb->Release();
        }
        cur = ts;
        showOne = seeking = false;
        if (duration > 0) progress_ = (float)std::clamp((double)ts / duration, 0.0, 1.0);
        sample->Release();
    }
    audio.Close();
    SafeRelease(reader);
    MFShutdown();
    CoUninitialize();
}
