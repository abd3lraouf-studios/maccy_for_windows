#pragma once

#include "ClipboardData.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace ClipboardRules {

namespace Limits {
    constexpr size_t kMaximumFormats = 4096;
    constexpr size_t kMaximumFormatNameBytes = 1024 * 1024;
    constexpr std::uint64_t kMaximumPayloadBytes = 256ULL * 1024ULL * 1024ULL;
    constexpr size_t kMaximumClipboardCharacters = 1024 * 1024;
    constexpr size_t kMaximumClipboardBytes = 32 * 1024 * 1024;
    constexpr std::uint64_t kMaximumImagePixels = 64ULL * 1024ULL * 1024ULL;
    constexpr size_t kMaximumPreviewTextCharacters = 4ULL * 1024ULL * 1024ULL;
    // Bound std::wregex haystacks to limit ReDoS / scan cost (M-10 / M-11).
    constexpr size_t kMaximumRegexHaystackCharacters = 64 * 1024;
    constexpr size_t kMaximumSearchListPreviewCharacters = 4096;
}

bool RegexSearchBounded(const std::wregex &expression, std::wstring_view text);
std::wstring_view BoundedRegexHaystack(std::wstring_view text);

const std::vector<std::wstring> &DefaultIgnoredFormats();

std::wstring Lower(std::wstring_view value);
std::wstring TrimWhitespace(std::wstring value);
bool EqualInsensitive(std::wstring_view lhs, std::wstring_view rhs);
std::wstring NormalizePath(std::wstring value);
bool MatchesApplication(std::wstring_view actual, std::wstring_view configured);

std::wstring MakeTitle(std::wstring value, bool show_special_symbols);
std::wstring StoredPreview(std::wstring value);
std::wstring PreviewText(std::wstring_view value);

std::wstring FormatName(UINT format);
bool IsClipboardViewerIgnoreFormat(UINT format, std::wstring_view name);
bool MatchesIgnoredFormatName(std::wstring_view actual, std::wstring_view configured);
bool IsUnicodeTextFormat(const ClipboardFormatData &data);
bool IsAnsiTextFormat(const ClipboardFormatData &data);
bool IsRichTextFormat(const ClipboardFormatData &data);
bool IsFilesFormat(const ClipboardFormatData &data);
bool IsTextFormat(UINT format, std::wstring_view name);
// Unformatted text only: unlike IsTextFormat, HTML and RTF do not count.
bool IsPlainTextFormat(const ClipboardFormatData &data);
bool IsImageFormat(UINT format, std::wstring_view name);
bool IsEncodedImageName(std::wstring_view name);
bool IsDib(const ClipboardFormatData &data);

std::wstring DecodeUnicodeText(
    const std::vector<unsigned char> &bytes,
    size_t maximum_characters = std::wstring::npos
);
std::wstring DecodeAnsiText(
    const std::vector<unsigned char> &bytes,
    size_t maximum_characters = std::wstring::npos
);
std::wstring DecodeByteText(
    const std::vector<unsigned char> &bytes,
    size_t maximum_characters = std::wstring::npos
);
// Markup to text; `value` is HTML only, without a CF_HTML header.
std::wstring StripHtml(std::wstring value);
// Text of a raw "HTML Format" (CF_HTML) payload: its fragment, else its
// document, located by the header's byte offsets.
std::wstring HtmlFormatText(const std::vector<unsigned char> &bytes);
std::wstring StripRtf(std::wstring_view value);
std::wstring ExtractDropPaths(const std::vector<unsigned char> &bytes);
std::vector<std::wstring> DecodeDropPaths(const std::vector<unsigned char> &bytes);

struct SearchParts {
    std::wstring body;
    std::wstring paths;
};

SearchParts SearchPartsFromFormats(const std::vector<ClipboardFormatData> &data);

// Content identity of a set of formats, independent of their order. History
// rows are deduplicated by it, so every writer of history_items.fingerprint
// must use this one hash.
std::wstring Fingerprint(const std::vector<ClipboardFormatData> &data);

// A NUL-terminated CF_UNICODETEXT payload holding `text`.
ClipboardFormatData UnicodeTextFormat(std::wstring_view text);

// The formats "paste without formatting" writes: plain text and file lists.
// An item with only rich text falls back to its stripped text. nullopt when
// the item has no text at all, so it is pasted unchanged.
std::optional<std::vector<ClipboardFormatData>> PlainPasteFormats(
    const std::vector<ClipboardFormatData> &data
);

} // namespace ClipboardRules
