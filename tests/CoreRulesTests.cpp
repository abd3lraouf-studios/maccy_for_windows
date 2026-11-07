#include "ClipboardRules.h"
#include "PinKeys.h"

#include <algorithm>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool CheckApplicationMatching() {
    return ClipboardRules::MatchesApplication(
               L"C:\\Program Files\\Editor\\Editor.exe",
               L"c:\\program files\\editor\\editor.exe") &&
        ClipboardRules::MatchesApplication(
               L"C:\\Program Files\\Editor\\Editor.exe",
               L"EDITOR.EXE") &&
        !ClipboardRules::MatchesApplication(
               L"C:\\Program Files\\Editor\\Editor.exe",
               L"Other\\Editor.exe") &&
        !ClipboardRules::MatchesApplication(L"", L"editor.exe");
}

bool CheckBoundedDecoding() {
    constexpr size_t limit = 4;
    const std::vector<unsigned char> unicode_bytes = {
        'A', 0, 'B', 0, 'C', 0, 'D', 0, 'E', 0, 0, 0,
    };
    const std::vector<unsigned char> nul_unicode_bytes = {
        'A', 0, 0, 0, 'B', 0, 0, 0,
    };
    const std::vector<unsigned char> ansi_bytes = {'A', 'B', 'C', 'D', 'E', 0};

    return ClipboardRules::DecodeUnicodeText(unicode_bytes, limit) == L"ABCD" &&
        ClipboardRules::DecodeUnicodeText(unicode_bytes, 0).empty() &&
        ClipboardRules::DecodeUnicodeText(nul_unicode_bytes, limit) == L"A" &&
        ClipboardRules::DecodeAnsiText(ansi_bytes, limit) == L"ABCD" &&
        ClipboardRules::DecodeAnsiText(ansi_bytes, 0).empty() &&
        ClipboardRules::DecodeAnsiText(ansi_bytes) == L"ABCDE";
}

bool CheckSearchParts() {
    const std::wstring text = L"clipboard text";
    std::vector<unsigned char> bytes((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(bytes.data(), text.c_str(), bytes.size());

    const ClipboardFormatData format{L"CF_UNICODETEXT", CF_UNICODETEXT, std::move(bytes)};
    const ClipboardRules::SearchParts parts = ClipboardRules::SearchPartsFromFormats({format});
    return parts.body == text && parts.paths.empty();
}

bool CheckPinKeys() {
    AppSettings settings;
    std::vector<ClipboardItem> pins;
    ClipboardItem pin;
    pin.id = 1;
    pin.pinned = true;
    pin.pin = L"b";
    pins.push_back(pin);

    const auto available = PinKeyPolicy::Available(pins, settings);
    return !PinKeyPolicy::IsValid(L"b", pins, settings, 0) &&
        PinKeyPolicy::IsValid(L"b", pins, settings, 1) &&
        PinKeyPolicy::Next(pins, settings) == L"0" &&
        PinKeyPolicy::IsValid(L"9", pins, settings, 0) &&
        std::find(available.begin(), available.end(), L'p') == available.end();
}

bool CheckStripMarkup() {
    const std::wstring html =
        L"<html><script>ignore()</script><body>A&nbsp;&amp;&#65;&#x42;<style>x{}</style></body></html>";
    const std::wstring rtf = L"{\\rtf1\\ansi{\\b hi}\\par\\u65 ?\\'42}";
    return ClipboardRules::StripHtml(html).find(L"A &AB") != std::wstring::npos &&
        ClipboardRules::StripHtml(html).find(L"ignore") == std::wstring::npos &&
        ClipboardRules::StripRtf(rtf).find(L"hi") != std::wstring::npos &&
        ClipboardRules::StripRtf(rtf).find(L"A") != std::wstring::npos;
}

bool CheckRegexBoundAndIgnoreAliases() {
    try {
        const std::wregex expression(L"secret");
        std::wstring haystack(ClipboardRules::Limits::kMaximumRegexHaystackCharacters + 8, L'x');
        haystack.replace(0, 6, L"secret");
        if (!ClipboardRules::RegexSearchBounded(expression, haystack)) {
            return false;
        }
        haystack.assign(ClipboardRules::Limits::kMaximumRegexHaystackCharacters + 8, L'x');
        haystack.replace(ClipboardRules::Limits::kMaximumRegexHaystackCharacters + 1, 6, L"secret");
        if (ClipboardRules::RegexSearchBounded(expression, haystack)) {
            return false;
        }
    } catch (const std::regex_error &) {
        return false;
    }
    return ClipboardRules::MatchesIgnoredFormatName(
               L"Clipboard Viewer Ignore",
               L"CF_CLIPBOARD_VIEWER_IGNORE"
           ) &&
        ClipboardRules::IsClipboardViewerIgnoreFormat(0, L"CF_CLIPBOARD_VIEWER_IGNORE");
}

ClipboardFormatData ByteFormat(std::wstring name, UINT format, std::string_view text) {
    ClipboardFormatData data{std::move(name), format};
    data.bytes.assign(text.begin(), text.end());
    data.bytes.push_back(0);
    return data;
}

bool CheckPlainPasteFormats() {
    const UINT html_format = ::RegisterClipboardFormatW(L"HTML Format");
    const UINT rtf_format = ::RegisterClipboardFormatW(L"Rich Text Format");
    const std::vector<ClipboardFormatData> rich_copy{
        ClipboardRules::UnicodeTextFormat(L"plain"),
        ByteFormat(L"HTML Format", html_format, "<b>bold</b>"),
        ByteFormat(L"Rich Text Format", rtf_format, "{\\rtf1 {\\b bold}}"),
        ByteFormat(L"CF_TEXT", CF_TEXT, "plain"),
    };
    const auto plain = ClipboardRules::PlainPasteFormats(rich_copy);
    if (!plain || plain->size() != 2 ||
        !ClipboardRules::IsUnicodeTextFormat((*plain)[0]) ||
        !ClipboardRules::IsAnsiTextFormat((*plain)[1])) {
        return false;
    }

    // Rich text alone still pastes as its stripped text.
    const auto stripped = ClipboardRules::PlainPasteFormats({rich_copy[1]});
    if (!stripped || stripped->size() != 1 ||
        ClipboardRules::DecodeUnicodeText(stripped->front().bytes) != L"bold") {
        return false;
    }

    // Without any text there is nothing to strip, so the item pastes unchanged.
    const ClipboardFormatData image{L"PNG", 0xC123, {1, 2, 3}};
    return !ClipboardRules::PlainPasteFormats({image}).has_value();
}

std::vector<unsigned char> CfHtml(std::string_view before, std::string_view fragment, std::string_view after) {
    // Fixed-width offsets keep the header length independent of their values.
    const auto field = [](size_t value) {
        std::string digits = std::to_string(value);
        return std::string(10 - digits.size(), '0') + digits;
    };
    const auto header = [&](size_t start_html, size_t end_html, size_t start_fragment, size_t end_fragment) {
        return "Version:0.9\r\nStartHTML:" + field(start_html) + "\r\nEndHTML:" + field(end_html) +
            "\r\nStartFragment:" + field(start_fragment) + "\r\nEndFragment:" + field(end_fragment) + "\r\n";
    };
    const size_t start_html = header(0, 0, 0, 0).size();
    const size_t start_fragment = start_html + before.size();
    const size_t end_fragment = start_fragment + fragment.size();
    const size_t end_html = end_fragment + after.size();
    const std::string payload = header(start_html, end_html, start_fragment, end_fragment) +
        std::string(before) + std::string(fragment) + std::string(after);
    std::vector<unsigned char> bytes(payload.begin(), payload.end());
    bytes.push_back(0);
    return bytes;
}

bool CheckHtmlFormatText() {
    // The two-byte U+00E9 before the fragment makes byte and character offsets
    // differ; the blank line inside the fragment is content, not a header end.
    const std::wstring fragment = ClipboardRules::HtmlFormatText(CfHtml(
        "<html><body><p>Caf\xC3\xA9 menu</p><!--StartFragment-->",
        "First line\r\n\r\n<b>Second line \xC3\xA9</b>",
        "<!--EndFragment--></body></html>"
    ));
    if (fragment.find(L"First line") != 0 || fragment.find(L"Second line \u00e9") == std::wstring::npos ||
        fragment.find(L"Caf") != std::wstring::npos || fragment.find(L"Version") != std::wstring::npos) {
        return false;
    }

    // Unusable offsets fall back to the markup after the header.
    const std::string broken = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\n<html><body>Body text</body></html>";
    const std::vector<unsigned char> broken_bytes(broken.begin(), broken.end());
    if (ClipboardRules::HtmlFormatText(broken_bytes) != L"Body text") {
        return false;
    }

    const ClipboardFormatData html{L"HTML Format", 0xC100, CfHtml("<html><body>", "Copied", "</body></html>")};
    return ClipboardRules::SearchPartsFromFormats({html}).body == L"Copied";
}

bool CheckRegexMatchErrorIsNoMatch() {
    // Nested repetition over long text exceeds MSVC's match-time stack or
    // complexity limit; the raw call must throw for this check to mean anything.
    const std::wregex expression(L"(a|aa)*b");
    const std::wstring haystack(ClipboardRules::Limits::kMaximumRegexHaystackCharacters / 2, L'a');
    bool raw_throws = false;
    try {
        static_cast<void>(std::regex_search(haystack, expression));
    } catch (const std::regex_error &) {
        raw_throws = true;
    }
    try {
        return raw_throws && !ClipboardRules::RegexSearchBounded(expression, haystack);
    } catch (...) {
        return false;
    }
}

} // namespace

int main() {
    if (!CheckApplicationMatching()) return 1;
    if (!CheckSearchParts()) return 2;
    if (!CheckPinKeys()) return 3;
    if (!CheckBoundedDecoding()) return 4;
    if (!CheckStripMarkup()) return 5;
    if (!CheckRegexBoundAndIgnoreAliases()) return 6;
    if (!CheckRegexMatchErrorIsNoMatch()) return 7;
    if (!CheckPlainPasteFormats()) return 8;
    if (!CheckHtmlFormatText()) return 9;
    return 0;
}
