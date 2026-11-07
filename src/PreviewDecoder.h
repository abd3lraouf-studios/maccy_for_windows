#pragma once

#include "PlatformConfig.h"

#include <optional>
#include <thread>
#include <vector>

#include "ClipboardData.h"

// A 32-bit top-down DIB section with premultiplied BGRA pixels. Draw it with
// AlphaBlend and kBlend: BitBlt/StretchBlt ignore alpha and show transparent
// areas as black.
struct PreviewBitmap {
    static constexpr BLENDFUNCTION kBlend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    HBITMAP handle = nullptr;
    int width = 0;
    int height = 0;

    PreviewBitmap() = default;
    PreviewBitmap(HBITMAP value, int bitmap_width, int bitmap_height)
        : handle(value), width(bitmap_width), height(bitmap_height) {}

    ~PreviewBitmap();

    PreviewBitmap(const PreviewBitmap &) = delete;
    PreviewBitmap &operator=(const PreviewBitmap &) = delete;

    PreviewBitmap(PreviewBitmap &&other) noexcept;
    PreviewBitmap &operator=(PreviewBitmap &&other) noexcept;

    HBITMAP Release() noexcept;
};

std::optional<PreviewBitmap> DecodePreviewBitmap(
    const ClipboardItem &item,
    UINT maximum_width,
    UINT maximum_height,
    std::stop_token stop_token
);
