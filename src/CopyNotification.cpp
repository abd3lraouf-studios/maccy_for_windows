#include "CopyNotification.h"

#include "ClipboardRules.h"
#include "Constants.h"
#include "Localization.h"
#include "PreviewDecoder.h"
#include "resource.h"

#include <atlbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using winrt::Windows::Data::Xml::Dom::XmlDocument;
using winrt::Windows::Data::Xml::Dom::XmlElement;
using winrt::Windows::UI::Notifications::ToastNotification;
using winrt::Windows::UI::Notifications::ToastNotificationManager;
using winrt::Windows::UI::Notifications::ToastNotifier;

// The NSIS uninstaller deletes this key by name (CMakeLists.txt).
constexpr wchar_t kAppUserModelId[] = L"Maccy.ClipboardHistory";
constexpr wchar_t kAppUserModelKey[] = L"Software\\Classes\\AppUserModelId\\Maccy.ClipboardHistory";
constexpr wchar_t kDisplayName[] = L"Maccy";
// About three banner lines, so the cut is ours and not the system's.
constexpr std::size_t kMaxSnippetLength = 110;
// Notification art renders at up to 2x: a 48 px logo and a 364 px wide image.
constexpr int kLogoPixels = 96;
constexpr UINT kImageMaxWidth = 728;
constexpr UINT kImageMaxHeight = 546;

struct Notice {
    std::wstring preview;
    std::wstring application;
    bool has_text = false;
    bool has_image = false;
    bool has_files = false;
    // Only the image and file-drop formats the notification can show.
    std::vector<ClipboardFormatData> data;
};

HWND g_owner = nullptr;
std::mutex g_mutex;
std::condition_variable_any g_wake;
// A burst of copies shows only the newest one.
std::optional<Notice> g_pending;
// Counts ClearHistory calls. Bumped together with dropping g_pending, so a
// copy the worker took before a clear is known to belong to the cleared past.
std::atomic<std::uint64_t> g_clears{0};
// Held by the worker while it writes a thumbnail or shows a notification,
// and by ClearHistory while it deletes them: each happens wholly before or
// after a clear, never across it.
std::mutex g_clearMutex;
std::jthread g_thread;

std::filesystem::path DataDirectory() {
    PWSTR local_app_data = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local_app_data);
    std::filesystem::path path = SUCCEEDED(result) ? std::filesystem::path(local_app_data) / L"maccy" : std::filesystem::path{};
    ::CoTaskMemFree(local_app_data);
    return path;
}

// IconUri must name an image file, so the embedded app PNG is written out.
bool RegisterAppUserModelId(const std::filesystem::path &folder) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kAppUserModelKey, 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const auto set_string = [key](const wchar_t *name, const std::wstring &value) {
        return ::RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE *>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    const bool registered = set_string(L"DisplayName", kDisplayName);

    const HRSRC resource = ::FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_APP_MACCY_PNG), RT_RCDATA);
    const HGLOBAL loaded = resource != nullptr ? ::LoadResource(nullptr, resource) : nullptr;
    const void *bytes = loaded != nullptr ? ::LockResource(loaded) : nullptr;
    const DWORD size = resource != nullptr ? ::SizeofResource(nullptr, resource) : 0;
    const std::filesystem::path icon = folder / L"notification-icon.png";
    std::error_code error;
    if (bytes != nullptr && size != 0 && std::filesystem::file_size(icon, error) != size) {
        std::filesystem::create_directories(folder, error);
        std::ofstream(icon, std::ios::binary | std::ios::trunc)
            .write(static_cast<const char *>(bytes), static_cast<std::streamsize>(size));
    }
    if (std::filesystem::exists(icon, error)) {
        set_string(L"IconUri", icon.wstring());
    }
    ::RegCloseKey(key);
    return registered;
}

// Clicking a notification opens its app through shell:AppsFolder\<AppUserModelID>,
// which only resolves when a Start menu shortcut carries that ID; otherwise
// Windows reports it cannot find the app. Stamp the ID on the installer's
// shortcut to this executable. A second launch hands off to the running copy,
// which opens the popup.
void StampStartMenuShortcuts() {
    // PKEY_AppUserModel_ID, spelled out so no extra import library is needed.
    constexpr PROPERTYKEY kAppUserModelIdKey{
        {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
    std::wstring executable(MAX_PATH, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return;
    }
    executable.resize(length);
    PWSTR programs = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs);
    const std::filesystem::path root = SUCCEEDED(result) ? std::filesystem::path(programs) : std::filesystem::path{};
    ::CoTaskMemFree(programs);
    if (root.empty()) {
        return;
    }

    const auto stamp = [&](const std::filesystem::path &link) {
        CComPtr<IShellLinkW> shell_link;
        if (link.extension() != L".lnk" || FAILED(shell_link.CoCreateInstance(CLSID_ShellLink))) {
            return;
        }
        CComQIPtr<IPersistFile> file(shell_link);
        CComQIPtr<IPropertyStore> store(shell_link);
        std::wstring target(MAX_PATH, L'\0');
        std::error_code error;
        if (!file || !store || FAILED(file->Load(link.c_str(), STGM_READWRITE)) ||
            FAILED(shell_link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, 0)) ||
            !std::filesystem::equivalent(target.c_str(), executable, error)) {
            return;
        }
        PROPVARIANT current;
        ::PropVariantInit(&current);
        const bool stamped = SUCCEEDED(store->GetValue(kAppUserModelIdKey, &current)) &&
            current.vt == VT_LPWSTR && ::lstrcmpW(current.pwszVal, kAppUserModelId) == 0;
        ::PropVariantClear(&current);
        if (stamped) {
            return;
        }
        PROPVARIANT value{};
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<wchar_t *>(kAppUserModelId);
        if (SUCCEEDED(store->SetValue(kAppUserModelIdKey, value)) && SUCCEEDED(store->Commit())) {
            file->Save(link.c_str(), TRUE);
        }
    };
    // The installer puts the shortcut one folder down (Programs\maccy\maccy.lnk).
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.is_directory(error)) {
            for (const auto &child : std::filesystem::directory_iterator(entry.path(), error)) {
                stamp(child.path());
            }
        } else {
            stamp(entry.path());
        }
    }
}

// Decoded thumbnails are premultiplied BGRA, but DIB sources and some icons
// leave alpha at zero; writing those with alpha would make them invisible.
bool HasAlpha(HBITMAP bitmap) {
    DIBSECTION section{};
    if (::GetObjectW(bitmap, sizeof(section), &section) != sizeof(section) ||
        section.dsBm.bmBitsPixel != 32 || section.dsBm.bmBits == nullptr) {
        return false;
    }
    const auto *pixels = static_cast<const BYTE *>(section.dsBm.bmBits);
    const std::size_t length = static_cast<std::size_t>(section.dsBm.bmWidthBytes) *
        static_cast<std::size_t>(std::abs(section.dsBm.bmHeight));
    for (std::size_t index = 3; index < length; index += 4) {
        if (pixels[index] != 0) {
            return true;
        }
    }
    return false;
}

bool WritePng(IWICImagingFactory *factory, HBITMAP bitmap, const std::filesystem::path &path) {
    CComPtr<IWICBitmap> source;
    CComPtr<IWICFormatConverter> converter;
    CComPtr<IWICStream> stream;
    CComPtr<IWICBitmapEncoder> encoder;
    CComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr,
               HasAlpha(bitmap) ? WICBitmapUsePremultipliedAlpha : WICBitmapIgnoreAlpha, &source)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
               WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WriteSource(converter, nullptr)) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit());
}

// FileDescription is the name people know ("Google Chrome", "Notepad");
// ProductName is often the OS for inbox apps.
std::wstring ApplicationName(const std::wstring &application) {
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(application.c_str(), &handle);
    std::vector<BYTE> block(size);
    struct Translation {
        WORD language;
        WORD code_page;
    } *translations = nullptr;
    UINT length = 0;
    if (size != 0 && ::GetFileVersionInfoW(application.c_str(), 0, size, block.data()) &&
        ::VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                         reinterpret_cast<void **>(&translations), &length) &&
        length >= sizeof(Translation)) {
        wchar_t key[64];
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                   translations->language, translations->code_page);
        wchar_t *value = nullptr;
        UINT value_length = 0;
        if (::VerQueryValueW(block.data(), key, reinterpret_cast<void **>(&value), &value_length) &&
            value != nullptr && value_length > 1 && value[0] != L'\0') {
            return value;
        }
    }
    return std::filesystem::path(application).stem().wstring();
}

std::filesystem::path ApplicationIcon(IWICImagingFactory *factory, const std::wstring &application,
                                      const std::filesystem::path &folder) {
    std::wstring key = application;
    for (wchar_t &ch : key) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    wchar_t name[32];
    swprintf_s(name, L"%016llx.png", static_cast<unsigned long long>(std::hash<std::wstring>{}(key)));
    const std::filesystem::path path = folder / L"notification-icons" / name;
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return path;
    }
    std::filesystem::create_directories(path.parent_path(), error);

    CComPtr<IShellItemImageFactory> images;
    HBITMAP bitmap = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(application.c_str(), nullptr, IID_PPV_ARGS(&images))) ||
        FAILED(images->GetImage(SIZE{kLogoPixels, kLogoPixels},
                                SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap))) {
        return {};
    }
    const bool written = WritePng(factory, bitmap, path);
    ::DeleteObject(bitmap);
    return written ? path : std::filesystem::path{};
}

// Copied text can be a whole document: fold it to one paragraph and cut it
// where the banner would stop anyway.
std::wstring Snippet(std::wstring_view text) {
    std::wstring result;
    bool pending_space = false;
    for (const wchar_t ch : text) {
        if (std::iswspace(ch) || std::iswcntrl(ch)) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(L' ');
            pending_space = false;
        }
        result.push_back(ch);
        if (result.size() > kMaxSnippetLength) {
            result.resize(kMaxSnippetLength);
            if (IS_HIGH_SURROGATE(result.back())) {
                result.pop_back();
            }
            while (!result.empty() && result.back() == L' ') {
                result.pop_back();
            }
            result.push_back(L'…');
            break;
        }
    }
    return result;
}

std::wstring FilesSnippet(const std::vector<std::wstring> &files) {
    std::filesystem::path first(files.front());
    if (first.filename().empty()) {
        first = first.parent_path();
    }
    const std::wstring name = first.filename().empty() ? first.wstring() : first.filename().wstring();
    return files.size() == 1
        ? name
        : Localization::Format(IDS_NOTIFY_MORE_FILES, {name, std::to_wstring(files.size() - 1)});
}

// Every thumbnail gets a fresh file name: Windows caches notification images
// by URI. Only the newest two are kept on disk.
std::filesystem::path Thumbnail(IWICImagingFactory *factory, ClipboardItem item,
                                const std::filesystem::path &folder,
                                const std::filesystem::path &previous, std::uint64_t clears,
                                std::stop_token stop) {
    auto bitmap = DecodePreviewBitmap(item, kImageMaxWidth, kImageMaxHeight, stop);
    if (!bitmap) {
        return {};
    }
    std::lock_guard lock(g_clearMutex);
    if (g_clears != clears) {
        return {};
    }
    const std::filesystem::path images = folder / L"notification-images";
    std::error_code error;
    std::filesystem::create_directories(images, error);
    for (const auto &entry : std::filesystem::directory_iterator(images, error)) {
        if (entry.path() != previous) {
            std::filesystem::remove(entry.path(), error);
        }
    }
    const std::filesystem::path path = images / (std::to_wstring(::GetTickCount64()) + L".png");
    return WritePng(factory, bitmap->handle, path) ? path : std::filesystem::path{};
}

ToastNotification BuildToast(const std::wstring &title, const std::wstring &body,
                             const std::filesystem::path &logo, const std::filesystem::path &image) {
    XmlDocument xml;
    xml.LoadXml(L"<toast launch=\"open\"><visual><binding template=\"ToastGeneric\"/></visual></toast>");
    XmlElement binding = xml.GetElementsByTagName(L"binding").Item(0).as<XmlElement>();
    // InnerText and SetAttribute escape copied content; it never becomes markup.
    const auto add_text = [&](const std::wstring &value) {
        XmlElement text = xml.CreateElement(L"text");
        text.InnerText(winrt::hstring(value));
        binding.AppendChild(text);
    };
    const auto add_image = [&](const std::filesystem::path &file, const wchar_t *placement) {
        XmlElement element = xml.CreateElement(L"image");
        element.SetAttribute(L"src", winrt::hstring(L"file:///" + file.generic_wstring()));
        if (placement != nullptr) {
            element.SetAttribute(L"placement", placement);
        }
        binding.AppendChild(element);
    };
    add_text(title);
    if (!body.empty()) {
        add_text(body);
    }
    if (!logo.empty()) {
        add_image(logo, L"appLogoOverride");
    }
    if (!image.empty()) {
        add_image(image, nullptr);
    }

    ToastNotification toast(xml);
    // One entry in Action Center: each copy replaces the previous one.
    toast.Tag(L"copy");
    toast.Group(L"clipboard");
    toast.Activated([owner = g_owner](auto &&, auto &&) {
        ::PostMessageW(owner, AppConstants::kOpenHotkeyMessage, 0, 0);
    });
    return toast;
}

void ThreadMain(std::stop_token stop) {
    // An exception escaping a thread function terminates the process.
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        return;
    }
    {
        const std::filesystem::path folder = DataDirectory();
        ToastNotifier notifier{nullptr};
        try {
            if (!folder.empty() && RegisterAppUserModelId(folder)) {
                StampStartMenuShortcuts();
                notifier = ToastNotificationManager::CreateToastNotifier(kAppUserModelId);
            }
        } catch (...) {
            notifier = nullptr;
        }
        CComPtr<IWICImagingFactory> factory;
        ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        // Holding the shown toast keeps its Activated handler alive.
        ToastNotification shown{nullptr};
        std::filesystem::path previous_image;

        while (notifier && factory) {
            Notice notice;
            std::uint64_t clears = 0;
            {
                std::unique_lock lock(g_mutex);
                if (!g_wake.wait(lock, stop, [] { return g_pending.has_value(); })) {
                    break;
                }
                notice = std::move(*g_pending);
                g_pending.reset();
                clears = g_clears;
            }
            try {
                std::vector<std::wstring> files;
                for (const ClipboardFormatData &format : notice.data) {
                    if (ClipboardRules::IsFilesFormat(format)) {
                        files = ClipboardRules::DecodeDropPaths(format.bytes);
                        break;
                    }
                }
                const std::wstring app_name =
                    notice.application.empty() ? std::wstring{} : ApplicationName(notice.application);
                const std::wstring title = app_name.empty()
                    ? Localization::Text(IDS_NOTIFY_COPIED)
                    : Localization::Format(IDS_NOTIFY_COPIED_FROM, {app_name});
                const std::wstring body = !files.empty()
                    ? FilesSnippet(files)
                    : (notice.has_image && !notice.has_text
                        ? Localization::Text(IDS_NOTIFY_IMAGE)
                        : Snippet(notice.preview));
                // Keep the layout steady when the source is unknown or has no icon.
                std::filesystem::path logo = notice.application.empty()
                    ? std::filesystem::path{}
                    : ApplicationIcon(factory, notice.application, folder);
                if (logo.empty()) {
                    logo = folder / L"notification-icon.png";
                }

                std::filesystem::path image;
                if (files.size() == 1 || (files.empty() && notice.has_image && !notice.has_text)) {
                    ClipboardItem item;
                    item.has_image = notice.has_image;
                    item.has_files = !files.empty();
                    item.file_paths = std::move(files);
                    item.data = std::move(notice.data);
                    image = Thumbnail(factory, std::move(item), folder, previous_image, clears, stop);
                    if (!image.empty()) {
                        previous_image = image;
                    }
                }

                ToastNotification toast = BuildToast(title, body, logo, image);
                std::lock_guard lock(g_clearMutex);
                if (g_clears == clears) {
                    notifier.Show(toast);
                    shown = toast;
                }
            } catch (...) {
                // A notification is best effort; the copy is already in history.
            }
        }
    }
    winrt::uninit_apartment();
}

} // namespace

namespace CopyNotification {

void Initialize(HWND owner) {
    Shutdown();
    // Match the ID stamped on the Start menu shortcut, so Maccy's windows,
    // its Start entry and its notifications are one app to the shell.
    ::SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
    g_owner = owner;
    g_thread = std::jthread(ThreadMain);
}

void Show(const ClipboardSnapshot &capture) {
    if (!g_thread.joinable()) {
        return;
    }
    Notice notice;
    notice.preview = capture.preview;
    notice.application = capture.application;
    notice.has_text = capture.has_text;
    notice.has_image = capture.has_image;
    notice.has_files = capture.has_files;
    const bool wants_image = capture.has_image && !capture.has_text;
    for (const ClipboardFormatData &format : capture.data) {
        if (ClipboardRules::IsFilesFormat(format) ||
            (wants_image && (ClipboardRules::IsEncodedImageName(format.name) || ClipboardRules::IsDib(format)))) {
            notice.data.push_back(format);
        }
    }
    {
        std::lock_guard lock(g_mutex);
        g_pending = std::move(notice);
    }
    g_wake.notify_one();
}

void ClearHistory() {
    {
        std::lock_guard lock(g_mutex);
        g_pending.reset();
        ++g_clears;
    }
    std::lock_guard lock(g_clearMutex);
    // Images of earlier runs too: the worker only prunes when it writes.
    if (const std::filesystem::path folder = DataDirectory(); !folder.empty()) {
        std::error_code error;
        std::filesystem::remove_all(folder / L"notification-images", error);
    }
    // Action Center keeps the last notification, snippet and image included.
    try {
        ToastNotificationManager::History().Clear(kAppUserModelId);
    } catch (...) {
        // Nothing was shown if notifications never registered.
    }
}

void Shutdown() noexcept {
    if (g_thread.joinable()) {
        g_thread.request_stop();
        g_thread.join();
    }
    std::lock_guard lock(g_mutex);
    g_pending.reset();
    g_owner = nullptr;
}

} // namespace CopyNotification
