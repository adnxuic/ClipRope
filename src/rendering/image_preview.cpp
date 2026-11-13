#include "rendering/image_preview.hpp"
#ifdef CLIPROPE_THUMBNAIL_PROBES
#include "thumbnail_gate.hpp"
#endif
#include "platform_win/media.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace cliprope::rendering {
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<std::uint64_t> decode_attempts{};
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Image codec could not decode this representation");
}
struct Image {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    UINT width{}, height{};
    WICBitmapTransformOptions orientation = WICBitmapTransformRotate0;
    Image(UINT format, std::span<const unsigned char> bytes) {
        if (bytes.empty() || bytes.size() > media::byte_limit(format))
            throw std::runtime_error("Image size out of range");
        check(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        if (format == RegisterClipboardFormatW(L"PNG")) {
            ComPtr<IWICStream> stream;
            check(factory->CreateStream(&stream));
            check(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                               static_cast<DWORD>(bytes.size())));
            ComPtr<IWICBitmapDecoder> decoder;
            check(
                CoCreateInstance(CLSID_WICPngDecoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder)));
            check(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand));
            ComPtr<IWICBitmapFrameDecode> frame;
            check(decoder->GetFrame(0, &frame));
            source = frame;
        } else {
            auto dib = media::inspect_dib(format, bytes);
            if (!dib)
                throw std::runtime_error("Unsupported packed bitmap");
            ComPtr<IWICBitmap> bitmap;
            const auto& pixel =
                dib->bits == 24 ? GUID_WICPixelFormat24bppBGR
                                : (dib->alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppBGR);
            check(factory->CreateBitmapFromMemory(dib->width, dib->height, pixel, dib->stride,
                                                  static_cast<UINT>(dib->size - dib->offset),
                                                  const_cast<BYTE*>(bytes.data() + dib->offset), &bitmap));
            if (dib->bottom_up) {
                ComPtr<IWICBitmapFlipRotator> flip;
                check(factory->CreateBitmapFlipRotator(&flip));
                check(flip->Initialize(bitmap.Get(), WICBitmapTransformFlipVertical));
                source = flip;
            } else
                source = bitmap;
        }
        check(source->GetSize(&width, &height));
        if (!width || !height || width > media::max_side || height > media::max_side ||
            width > media::max_pixels / height)
            throw std::runtime_error("Decoded image size out of range");
    }
    explicit Image(const std::wstring& path) {
        const auto dot = path.find_last_of(L'.');
        if (dot == path.npos) throw std::runtime_error("Unknown image extension");
        const auto* ext = path.c_str() + dot;
        const CLSID* codec = nullptr;
        if (!_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg")) codec = &CLSID_WICJpegDecoder;
        else if (!_wcsicmp(ext, L".png")) codec = &CLSID_WICPngDecoder;
        else if (!_wcsicmp(ext, L".bmp")) codec = &CLSID_WICBmpDecoder;
        if (!codec) throw std::runtime_error("Unsupported image file");
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        ComPtr<IWICStream> stream;
        check(factory->CreateStream(&stream));
        check(stream->InitializeFromFilename(path.c_str(), GENERIC_READ));
        ComPtr<IWICBitmapDecoder> decoder;
        check(CoCreateInstance(*codec, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder)));
#ifdef CLIPROPE_THUMBNAIL_PROBES
        thumbnail_fixture::pause_at(L"codec");
#endif
        check(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand));
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(0, &frame));
        source = frame;
        check(frame->GetSize(&width, &height));
        // Bound codec working memory even when only a small thumbnail was requested.
        if (!width || !height || width > 32768 || height > 32768 || width > (40u * 1024 * 1024) / height)
            throw std::runtime_error("Image file exceeds decode limits");
        if (codec == &CLSID_WICJpegDecoder) {
            ComPtr<IWICMetadataQueryReader> metadata;
            PROPVARIANT value{};
            if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata)) &&
                SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) &&
                value.vt == VT_UI2 && value.uiVal >= 2 && value.uiVal <= 8) {
                constexpr unsigned exif[]{0, 0, WICBitmapTransformFlipHorizontal, WICBitmapTransformRotate180,
                    WICBitmapTransformFlipVertical, WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal,
                    WICBitmapTransformRotate90, WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal,
                    WICBitmapTransformRotate270};
                orientation = static_cast<WICBitmapTransformOptions>(exif[value.uiVal]);
            }
            PropVariantClear(&value);
        }
    }
    void orient() {
        if (orientation == WICBitmapTransformRotate0) return;
        ComPtr<IWICBitmapFlipRotator> rotate;
        check(factory->CreateBitmapFlipRotator(&rotate));
        check(rotate->Initialize(source.Get(), orientation));
        source = rotate;
        check(source->GetSize(&width, &height));
    }
    void convert(REFWICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA) {
        ComPtr<IWICFormatConverter> converter;
        check(factory->CreateFormatConverter(&converter));
        check(converter->Initialize(source.Get(), format, WICBitmapDitherTypeNone,
                                    nullptr, 0, WICBitmapPaletteTypeCustom));
        source = converter;
    }
};
} // namespace
bool validate_png_pixels(std::span<const unsigned char> bytes) {
    try {
        Image image(RegisterClipboardFormatW(L"PNG"), bytes);
        image.convert();
        const UINT bands = std::min(32u, image.height), stride = image.width * 4;
        std::vector<BYTE> scratch(stride * bands);
        for (UINT y = 0; y < image.height; y += bands) {
            WICRect rect{0, static_cast<int>(y), static_cast<int>(image.width),
                         static_cast<int>(std::min(bands, image.height - y))};
            check(image.source->CopyPixels(&rect, stride, static_cast<UINT>(scratch.size()), scratch.data()));
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
static ImagePreview scale_preview(Image image, unsigned max_width, unsigned max_height, bool enlarge = false,
                                const MetadataReady& measured = {}, unsigned limit = 1024) {
    if (!max_width || !max_height || max_width > limit || max_height > limit)
        throw std::invalid_argument("Invalid thumbnail bounds");
    // Scale in the codec's native orientation so rotating a thumbnail never
    // requires a full-resolution intermediate.
    domain::ImageMetadata source{image.width, image.height, unsigned(image.orientation)};
    if (source.orientation & 1u) {
        std::swap(source.width, source.height);
        std::swap(max_width, max_height);
    }
    if (measured) measured(source);
    auto ratio = std::min(double(max_width) / image.width, double(max_height) / image.height);
    if (!enlarge) ratio = std::min(1., ratio);
    auto width = std::max(1u, static_cast<unsigned>(std::lround(image.width * ratio)));
    auto height = std::max(1u, static_cast<unsigned>(std::lround(image.height * ratio)));
    if (width != image.width || height != image.height) {
        ComPtr<IWICBitmapScaler> scaler;
        check(image.factory->CreateBitmapScaler(&scaler));
        check(scaler->Initialize(image.source.Get(), width, height, WICBitmapInterpolationModeFant));
        image.source = scaler;
    }
    image.orient();
    check(image.source->GetSize(&width, &height));
    image.convert();
    ImagePreview result{width, height, std::vector<BYTE>(width * height * 4), source};
    check(image.source->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.pbgra.size()),
                                   result.pbgra.data()));
    return result;
}
std::uint64_t preview_decodes() { return decode_attempts.load(std::memory_order_relaxed); }
void record_preview_decodes(unsigned count) { decode_attempts.fetch_add(count, std::memory_order_relaxed); }
ImagePreview decode_preview(UINT format, std::span<const unsigned char> bytes, unsigned w, unsigned h) {
    decode_attempts.fetch_add(1,std::memory_order_relaxed);
    return scale_preview(Image(format, bytes), w, h);
}
ImagePreview decode_screenshot_preview(UINT format, std::span<const unsigned char> bytes, unsigned w, unsigned h) {
    if (!w || !h || w > 2048 || h > 2048 || std::uint64_t(w) * h > 2097152 ||
        !media::screenshot_header(format, bytes)) throw std::invalid_argument("Invalid screenshot preview bounds");
    return scale_preview(Image(format, bytes), w, h, false, {}, 2048);
}
std::vector<unsigned char> encode_dib_png(UINT format, std::span<const unsigned char> bytes) {
    Image image(format, bytes);
    ComPtr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    check(image.factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr));
    check(frame->Initialize(nullptr));
    check(frame->SetSize(image.width, image.height));
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&pixel_format));
    image.convert(pixel_format);
    check(frame->WriteSource(image.source.Get(), nullptr));
    check(frame->Commit());
    check(encoder->Commit());

    STATSTG stat{};
    check(stream->Stat(&stat, STATFLAG_NONAME));
    // A PNG of an incompressible, bounded DIB may be slightly larger than its pixels.
    if (!stat.cbSize.QuadPart || stat.cbSize.QuadPart > media::byte_limit(CF_DIBV5) + 1024u * 1024u)
        throw std::runtime_error("Screenshot PNG exceeds output limit");
    const auto size = static_cast<ULONG>(stat.cbSize.QuadPart);
    std::vector<unsigned char> png(size);
    check(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    ULONG read{};
    check(stream->Read(png.data(), size, &read));
    if (read != size) throw std::runtime_error("Incomplete screenshot PNG");
    return png;
}

ImagePreview decode_file_preview(const std::wstring& path, unsigned w, unsigned h, bool enlarge,
                                 const MetadataReady& measured) {
    decode_attempts.fetch_add(1,std::memory_order_relaxed);
    return scale_preview(Image(path), w, h, enlarge, measured);
}
bool matches_image_file(const std::wstring& path, UINT format, std::span<const unsigned char> bytes, std::stop_token stop) {
    try {
        Image saved(format, bytes), candidate(path);
        candidate.orient();
        if (saved.width != candidate.width || saved.height != candidate.height) return false;
        saved.convert(GUID_WICPixelFormat32bppBGRA);
        candidate.convert(GUID_WICPixelFormat32bppBGRA);
        const UINT stride = saved.width * 4, band = std::min(32u, saved.height);
        std::vector<BYTE> left(stride * band), right(left.size());
        for (UINT y = 0; y < saved.height; y += band) {
            if (stop.stop_requested()) return false;
            WICRect rect{0, static_cast<int>(y), static_cast<int>(saved.width),
                         static_cast<int>(std::min(band, saved.height - y))};
            const UINT length = stride * rect.Height;
            check(saved.source->CopyPixels(&rect, stride, length, left.data()));
            check(candidate.source->CopyPixels(&rect, stride, length, right.data()));
            if (std::memcmp(left.data(), right.data(), length)) return false;
        }
        return true;
    } catch (const std::exception&) { return false; }
}
} // namespace cliprope::rendering
