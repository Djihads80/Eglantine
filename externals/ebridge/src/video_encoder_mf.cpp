#ifdef _WIN32

#include "video_encoder_mf.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <initguid.h>
#include <codecapi.h>

#include "video_encoder.h"

namespace ebridge {

namespace {

template <typename T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string Hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08lx", static_cast<unsigned long>(hr));
    return buf;
}

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Counts the slices of one Annex-B access unit and tells whether it has an SPS.
void ScanNals(const std::vector<uint8_t>& au, int* slices, bool* has_sps) {
    *slices = 0;
    *has_sps = false;
    const size_t n = au.size();
    for (size_t i = 0; i + 3 < n; i++) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const int type = au[i + 3] & 0x1f;
            if (type == 1 || type == 5) ++*slices;
            if (type == 7) *has_sps = true;
            i += 2;
        }
    }
}

// Drops filler NAL units (type 12): CBR encoders pad with them, and they would
// only cost bandwidth on the way to the console.
void StripFiller(std::vector<uint8_t>& au) {
    std::vector<size_t> starts;   // offset of each start code
    for (size_t i = 0; i + 3 < au.size(); i++) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            starts.push_back(i > 0 && au[i - 1] == 0 ? i - 1 : i);
            i += 2;
        }
    }
    bool any = false;
    for (size_t s : starts) {
        const size_t hdr = au[s + 2] == 1 ? s + 3 : s + 4;
        if ((au[hdr] & 0x1f) == 12) any = true;
    }
    if (!any) return;
    std::vector<uint8_t> kept;
    kept.reserve(au.size());
    for (size_t k = 0; k < starts.size(); k++) {
        const size_t s = starts[k];
        const size_t e = k + 1 < starts.size() ? starts[k + 1] : au.size();
        const size_t hdr = au[s + 2] == 1 ? s + 3 : s + 4;
        if ((au[hdr] & 0x1f) != 12) kept.insert(kept.end(), au.begin() + s, au.begin() + e);
    }
    au.swap(kept);
}

// IID of ICodecAPI, spelled out: MinGW has no __uuidof for it.
constexpr GUID kIidCodecApi = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

bool SetCodecValue(ICodecAPI* api, const GUID& key, ULONG value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    return SUCCEEDED(api->SetValue(&key, &v));
}

} // namespace

struct MfEncoder::Impl {
    bool mf_started = false;
    bool com_started = false;
    IMFTransform* mft = nullptr;
    ICodecAPI* codec = nullptr;
    IMFMediaEventGenerator* events = nullptr;   // only for asynchronous (hardware) encoders
    DWORD in_id = 0, out_id = 0;
    bool provides_samples = false;
    DWORD out_size = 0;
    int width = 0, height = 0, fps = 60;
    int64_t pts = 0;
    int need_input = 0;            // NeedInput events not yet answered (asynchronous only)
    bool idr_next = true;
    std::vector<uint8_t> header;   // SPS + PPS, put in front of keyframes that lack them

    ~Impl() { Free(); }

    void Free() {
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        Release(events);
        Release(codec);
        Release(mft);
        if (mf_started) MFShutdown();
        if (com_started) CoUninitialize();
        mf_started = com_started = false;
        need_input = 0;
        pts = 0;
        header.clear();
    }

    bool SetTypes(int bitrate_kbps, std::string* error) {
        IMFMediaType* out = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate_kbps) * 1000);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        HRESULT hr = mft->SetOutputType(out_id, out, 0);
        Release(out);
        if (FAILED(hr)) {
            *error = "output type " + Hex(hr);
            return false;
        }

        IMFMediaType* in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
        in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        hr = mft->SetInputType(in_id, in, 0);
        Release(in);
        if (FAILED(hr)) {
            *error = "input type " + Hex(hr);
            return false;
        }
        return true;
    }

    void Configure(int bitrate_kbps) {
        if (!codec) return;
        SetCodecValue(codec, CODECAPI_AVLowLatencyMode, 1);
        SetCodecValue(codec, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetCodecValue(codec, CODECAPI_AVEncCommonMeanBitRate, static_cast<ULONG>(bitrate_kbps) * 1000);
        SetCodecValue(codec, CODECAPI_AVEncMPVGOPSize, static_cast<ULONG>(fps * 5));
        SetCodecValue(codec, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetCodecValue(codec, CODECAPI_AVEncCommonQualityVsSpeed, 0);   // fastest
        // 4 slices so the Switch decodes with its 4 cores: rows of macroblocks per slice.
        const ULONG mb_rows = static_cast<ULONG>((height + 15) / 16);
        if (!SetCodecValue(codec, CODECAPI_AVEncSliceControlMode, 2) ||
            !SetCodecValue(codec, CODECAPI_AVEncSliceControlSize, (mb_rows + 3) / 4)) {
            // Some encoders only take a size in macroblocks.
            const ULONG mbs = mb_rows * static_cast<ULONG>((width + 15) / 16);
            SetCodecValue(codec, CODECAPI_AVEncSliceControlMode, 0);
            SetCodecValue(codec, CODECAPI_AVEncSliceControlSize, (mbs + 3) / 4);
        }
    }

    bool OutputTypeChanged() {
        IMFMediaType* type = nullptr;
        if (FAILED(mft->GetOutputAvailableType(out_id, 0, &type))) return false;
        const bool ok = SUCCEEDED(mft->SetOutputType(out_id, type, 0));
        Release(type);
        return ok;
    }

    // Reads every finished frame. Returns false on an encoder error.
    bool Drain(std::vector<EncodedPacket>& out) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER buf{};
            buf.dwStreamID = out_id;
            IMFSample* own = nullptr;
            if (!provides_samples) {
                IMFMediaBuffer* mb = nullptr;
                if (FAILED(MFCreateSample(&own)) ||
                    FAILED(MFCreateMemoryBuffer(out_size ? out_size : width * height * 2, &mb)))
                    return false;
                own->AddBuffer(mb);
                Release(mb);
                buf.pSample = own;
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &buf, &status);
            if (buf.pEvents) buf.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                Release(own);
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                Release(own);
                if (!OutputTypeChanged()) return false;
                continue;
            }
            if (FAILED(hr)) {
                Release(own);
                return false;
            }
            IMFSample* sample = buf.pSample;
            EncodedPacket p;
            IMFMediaBuffer* mb = nullptr;
            if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&mb))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
                    p.data.assign(data, data + len);
                    mb->Unlock();
                }
                Release(mb);
                UINT32 clean = 0;
                p.key = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;
            }
            if (provides_samples && sample) sample->Release();
            Release(own);
            StripFiller(p.data);
            if (!p.data.empty()) {
                int slices;
                bool has_sps;
                ScanNals(p.data, &slices, &has_sps);
                if (p.key && !has_sps && !header.empty())
                    p.data.insert(p.data.begin(), header.begin(), header.end());
                out.push_back(std::move(p));
            }
            if (events) return true;   // asynchronous: one frame per HaveOutput event
        }
    }

    // Asynchronous encoders: handles the events that are already queued. With
    // wait_input, blocks until the encoder asks for input.
    bool Pump(bool wait_input, std::vector<EncodedPacket>& out) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        for (;;) {
            if (wait_input && need_input > 0) return true;
            IMFMediaEvent* ev = nullptr;
            const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (!wait_input) return true;
                if (std::chrono::steady_clock::now() > deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (FAILED(hr)) return false;
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            Release(ev);
            if (type == METransformNeedInput) {
                need_input++;
            } else if (type == METransformHaveOutput) {
                if (!Drain(out)) return false;
            }
        }
    }

    bool Feed(const uint8_t* nv12, bool force_idr, std::vector<EncodedPacket>& out) {
        if (events && !Pump(true, out)) return false;

        const DWORD size = static_cast<DWORD>(width * height * 3 / 2);
        IMFMediaBuffer* mb = nullptr;
        IMFSample* sample = nullptr;
        if (FAILED(MFCreateMemoryBuffer(size, &mb))) return false;
        BYTE* data = nullptr;
        mb->Lock(&data, nullptr, nullptr);
        std::memcpy(data, nv12, size);
        mb->Unlock();
        mb->SetCurrentLength(size);
        MFCreateSample(&sample);
        sample->AddBuffer(mb);
        Release(mb);
        const LONGLONG duration = 10000000LL / fps;
        sample->SetSampleTime(pts * duration);
        sample->SetSampleDuration(duration);
        pts++;

        if ((force_idr || idr_next) && codec) SetCodecValue(codec, CODECAPI_AVEncVideoForceKeyFrame, 1);
        idr_next = false;
        const HRESULT hr = mft->ProcessInput(in_id, sample, 0);
        Release(sample);
        if (FAILED(hr)) return false;
        if (events) {
            need_input--;
            // Low latency: wait a little for this frame's output.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            const size_t before = out.size();
            while (out.size() == before && std::chrono::steady_clock::now() < until) {
                if (!Pump(false, out)) return false;
                if (out.size() == before) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }
        return Drain(out);
    }

    bool Start(IMFActivate* act, int bitrate_kbps, std::string* error) {
        HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&mft));
        if (FAILED(hr)) {
            *error = "activate " + Hex(hr);
            return false;
        }
        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
            UINT32 async = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                mft->QueryInterface(IID_PPV_ARGS(&events));
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            Release(attrs);
        }
        mft->QueryInterface(kIidCodecApi, reinterpret_cast<void**>(&codec));
        DWORD in_count = 0, out_count = 0;
        if (SUCCEEDED(mft->GetStreamCount(&in_count, &out_count)) && in_count == 1 && out_count == 1 &&
            mft->GetStreamIDs(1, &in_id, 1, &out_id) == E_NOTIMPL) {
            in_id = out_id = 0;
        }
        Configure(bitrate_kbps);
        if (!SetTypes(bitrate_kbps, error)) return false;
        Configure(bitrate_kbps);   // some encoders reset the settings when the types change

        MFT_OUTPUT_STREAM_INFO info{};
        mft->GetOutputStreamInfo(out_id, &info);
        provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        out_size = info.cbSize;

        IMFMediaType* type = nullptr;
        if (SUCCEEDED(mft->GetOutputCurrentType(out_id, &type))) {
            UINT32 len = 0;
            if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &len)) && len) {
                header.resize(len);
                type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), len, nullptr);
            }
            Release(type);
        }
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // Encodes a few grey frames and counts the slices of the first one that comes
    // out. The stream restarts with a keyframe afterwards.
    int ProbeSlices() {
        std::vector<uint8_t> grey(static_cast<size_t>(width) * height * 3 / 2, 128);
        std::memset(grey.data(), 100, static_cast<size_t>(width) * height);
        std::vector<EncodedPacket> out;
        for (int i = 0; i < 8 && out.empty(); i++) {
            if (!Feed(grey.data(), i == 0, out)) return -1;
        }
        for (int i = 0; i < 50 && out.empty() && events; i++) {
            if (!Pump(false, out)) return -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (out.empty()) return -1;
        int slices;
        bool has_sps;
        ScanNals(out.front().data, &slices, &has_sps);
        idr_next = true;
        return slices;
    }
};

MfEncoder::MfEncoder() : impl(std::make_unique<Impl>()) {}
MfEncoder::~MfEncoder() = default;

bool MfEncoder::Open(int width, int height, int fps, int bitrate_kbps, bool hardware,
                     std::string* name, std::string* error) {
    auto& d = *impl;
    d.Free();
    d.width = width;
    d.height = height;
    d.fps = fps;
    d.idr_next = true;

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    d.com_started = SUCCEEDED(co);   // S_FALSE too: each success needs its CoUninitialize
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (error) *error = "Media Foundation not available";
        d.Free();
        return false;
    }
    d.mf_started = true;

    MFT_REGISTER_TYPE_INFO in_type{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_type{MFMediaType_Video, MFVideoFormat_H264};
    const UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in_type, &out_type, &list, &count)) ||
        count == 0) {
        if (error) *error = hardware ? "no GPU H.264 encoder" : "no H.264 encoder";
        if (list) CoTaskMemFree(list);
        d.Free();
        return false;
    }

    std::string errors;
    bool opened = false;
    for (UINT32 i = 0; i < count && !opened; i++) {
        wchar_t* wname = nullptr;
        UINT32 len = 0;
        list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wname, &len);
        const std::string friendly = wname ? Narrow(wname) : "encoder " + std::to_string(i);
        if (wname) CoTaskMemFree(wname);

        std::string err;
        if (d.Start(list[i], bitrate_kbps, &err)) {
            const int slices = d.ProbeSlices();
            if (slices >= 2) {
                if (name) *name = "Media Foundation " + friendly + " (" + std::to_string(slices) + " slices)";
                opened = true;
                break;
            }
            err = slices < 0 ? "no output" : "one slice per frame";
        }
        errors += friendly + ": " + err + "; ";
        // Keep MF running, drop only this encoder.
        if (d.mft) {
            d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            list[i]->ShutdownObject();
        }
        Release(d.events);
        Release(d.codec);
        Release(d.mft);
        d.need_input = 0;
        d.pts = 0;
        d.header.clear();
    }
    for (UINT32 i = 0; i < count; i++) list[i]->Release();
    CoTaskMemFree(list);
    if (!opened) {
        if (error) *error = errors;
        d.Free();
    }
    return opened;
}

bool MfEncoder::Encode(const uint8_t* nv12, bool force_idr, std::vector<EncodedPacket>& out) {
    return impl->mft && impl->Feed(nv12, force_idr, out);
}

} // namespace ebridge

#endif
