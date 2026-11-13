// Decode a single Annex B SPS/PPS/IDR capture using Windows Media Foundation.
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <cstdio>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;

static void check(HRESULT hr, const char *step)
{
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s failed (0x%08lx)\n", step, static_cast<unsigned long>(hr));
        throw hr;
    }
}

static void set_output(IMFTransform *decoder)
{
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> type;
        check(decoder->GetOutputAvailableType(0, i, &type), "find NV12 output");
        GUID subtype;
        check(type->GetGUID(MF_MT_SUBTYPE, &subtype), "get output subtype");
        if (subtype == MFVideoFormat_NV12) {
            check(decoder->SetOutputType(0, type.Get(), 0), "set NV12 output");
            return;
        }
    }
}

static void decode(const std::vector<char> &bytes, const wchar_t *output)
{
    ComPtr<IMFTransform> decoder;
    check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&decoder)), "create H.264 decoder");
    ComPtr<IMFMediaType> input;
    check(MFCreateMediaType(&input), "create H.264 input");
    check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "set input media type");
    check(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "set H.264 subtype");
    check(decoder->SetInputType(0, input.Get(), 0), "select H.264 input");
    set_output(decoder.Get());

    ComPtr<IMFSample> encoded;
    ComPtr<IMFMediaBuffer> compressed;
    const auto size = static_cast<DWORD>(bytes.size());
    check(MFCreateSample(&encoded), "create input sample");
    check(MFCreateMemoryBuffer(size, &compressed), "allocate compressed buffer");
    BYTE *data;
    check(compressed->Lock(&data, nullptr, nullptr), "lock input buffer");
    memcpy(data, bytes.data(), size);
    check(compressed->Unlock(), "unlock input buffer");
    check(compressed->SetCurrentLength(size), "set compressed length");
    check(encoded->AddBuffer(compressed.Get()), "add input buffer");
    check(encoded->SetSampleTime(0), "set input timestamp");
    check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "begin stream");
    check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "start stream");
    check(decoder->ProcessInput(0, encoded.Get(), 0), "submit IDR");
    check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "end input");
    check(decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "drain decoder");

    for (int attempt = 0; attempt < 4; ++attempt) {
        MFT_OUTPUT_STREAM_INFO info = {};
        check(decoder->GetOutputStreamInfo(0, &info), "get output buffer requirements");
        ComPtr<IMFSample> sample;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                              MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            ComPtr<IMFMediaBuffer> buffer;
            check(MFCreateSample(&sample), "create output sample");
            check(MFCreateMemoryBuffer(info.cbSize, &buffer), "allocate output buffer");
            check(sample->AddBuffer(buffer.Get()), "add output buffer");
        }
        MFT_OUTPUT_DATA_BUFFER result = {};
        result.pSample = sample.Get();
        DWORD flags;
        const auto hr = decoder->ProcessOutput(0, 1, &result, &flags);
        if (result.pEvents)
            result.pEvents->Release();
        if (result.pSample != sample.Get())
            sample.Attach(result.pSample);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            set_output(decoder.Get());
            continue;
        }
        check(hr, "decode IDR");
        if (!sample)
            continue;

        ComPtr<IMFMediaType> decoded;
        UINT32 width, height;
        check(decoder->GetOutputCurrentType(0, &decoded), "get decoded format");
        check(MFGetAttributeSize(decoded.Get(), MF_MT_FRAME_SIZE, &width, &height),
              "get decoded dimensions");
        DWORD length;
        check(sample->GetTotalLength(&length), "get decoded byte count");
        if (!width || !height || length < static_cast<UINT64>(width) * height * 3 / 2)
            check(E_FAIL, "validate decoded NV12 frame");
        std::printf("Decoded %u x %u NV12 frame (%lu bytes)\n", width, height, length);
        if (output) {
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer), "get output pixels");
            ComPtr<IMF2DBuffer> plane;
            DWORD count = length;
            const bool is2d = SUCCEEDED(buffer.As(&plane));
            if (is2d) check(plane->GetContiguousLength(&count), "get packed NV12 length");
            if (count != static_cast<UINT64>(width) * height * 3 / 2)
                check(E_FAIL, "validate packed NV12 dimensions");
            std::vector<BYTE> pixels(count);
            if (is2d) {
                check(plane->ContiguousCopyTo(pixels.data(), count), "copy packed NV12");
            } else {
                UINT32 stride = 0;
                check(decoded->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride), "get packed stride");
                if (stride != width) check(E_FAIL, "require packed stride");
                BYTE *raw = nullptr;
                DWORD actual = 0;
                check(buffer->Lock(&raw, nullptr, &actual), "lock output pixels");
                if (actual == count) memcpy(pixels.data(), raw, count);
                check(buffer->Unlock(), "unlock output pixels");
                if (actual != count) check(E_FAIL, "validate output length");
            }
            std::ofstream saved(output, std::ios::binary);
            saved.write(reinterpret_cast<const char *>(pixels.data()), count);
            if (!saved) check(E_FAIL, "save decoded pixels");
        }
        return;
    }
    check(E_FAIL, "obtain decoded frame");
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2 && argc != 3) {
        std::fputs("Usage: decode-first-idr-win.exe FIRST_IDR.h264 [OUTPUT.nv12]\n", stderr);
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0 || size > 2 * 1024 * 1024 + 2 * 4096) {
        std::fputs("Invalid or unreadable first-IDR capture\n", stderr);
        return 2;
    }
    std::vector<char> bytes(static_cast<size_t>(size));
    if (!file.seekg(0).read(bytes.data(), static_cast<std::streamsize>(size)))
        return 2;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 1;
    const auto startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    int status = 1;
    try {
        check(startup, "start Media Foundation");
        decode(bytes, argc == 3 ? argv[2] : nullptr);
        status = 0;
    } catch (HRESULT) {
    }
    if (SUCCEEDED(startup))
        MFShutdown();
    CoUninitialize();
    return status;
}
