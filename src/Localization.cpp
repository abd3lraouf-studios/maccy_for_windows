#include "Localization.h"

#include <string>

namespace Localization {

void Initialize() noexcept {
    // The app ships a single English string table, so there is no UI language
    // to select. Kept only because main.cpp still calls it at startup.
}

std::wstring FromUtf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    // Without MB_ERR_INVALID_CHARS, malformed bytes from server data become
    // U+FFFD instead of failing, so an update error can always be displayed.
    const int length = ::MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        length
    );
    return result;
}

std::wstring Text(UINT resource_id) {
    wchar_t buffer[1024] = {};
    const int length = ::LoadStringW(
        ::GetModuleHandleW(nullptr),
        resource_id,
        buffer,
        ARRAYSIZE(buffer)
    );
    return length > 0 ? std::wstring(buffer, static_cast<size_t>(length)) : std::wstring{};
}

std::wstring Format(
    UINT resource_id,
    std::initializer_list<std::wstring_view> values
) {
    std::wstring result = Text(resource_id);
    size_t index = 0;
    for (const std::wstring_view value : values) {
        const std::wstring placeholder = L"{" + std::to_wstring(index++) + L"}";
        size_t position = 0;
        while ((position = result.find(placeholder, position)) != std::wstring::npos) {
            result.replace(position, placeholder.size(), value.data(), value.size());
            position += value.size();
        }
    }
    return result;
}

} // namespace Localization
