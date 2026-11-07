#include "UpdateChecker.h"

#include "Constants.h"
#include "Localization.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <boost/system/system_error.hpp>

#include <openssl/ssl.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The one place the fork's repository is named: the API endpoint, the release-URL
// guard and the fallback page are all derived from it, so they cannot drift apart.
#define MACCY_GITHUB_REPOSITORY "abd3lraouf-studios/maccy_for_windows"
#define MACCY_GITHUB_API_HOST "api.github.com"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using Deadline = std::chrono::steady_clock::time_point;

constexpr char kGitHubHost[] = MACCY_GITHUB_API_HOST;
constexpr wchar_t kGitHubHostWide[] = L"" MACCY_GITHUB_API_HOST;
constexpr wchar_t kGitHubPort[] = L"443";
constexpr char kGitHubLatestReleaseTarget[] =
    "/repos/" MACCY_GITHUB_REPOSITORY "/releases/latest";
constexpr std::string_view kGitHubReleaseTagUrlPrefix =
    "https://github.com/" MACCY_GITHUB_REPOSITORY "/releases/tag/";
constexpr wchar_t kGitHubReleasePage[] =
    L"https://github.com/" MACCY_GITHUB_REPOSITORY "/releases/latest";
constexpr std::size_t kMaximumResponseBytes = 4 * 1024 * 1024;
// One budget for the whole check, from DNS to the last body byte. Stop() does not
// wait for it: it cancels the step in progress instead.
constexpr auto kCheckTimeout = std::chrono::seconds(15);

class LocalizedUpdateError final : public std::exception {
public:
    explicit LocalizedUpdateError(std::wstring message)
        : m_message(std::move(message)) {}

    const std::wstring &Message() const noexcept { return m_message; }

    const char *what() const noexcept override {
        return "localized update check error";
    }

private:
    std::wstring m_message;
};

struct SemanticVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

struct HttpResponse {
    unsigned status = 0;
    std::string body;
};

std::optional<SemanticVersion> ParseSemanticVersion(std::wstring_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::size_t position = 0;
    if (value[position] == L'v' || value[position] == L'V') {
        ++position;
    }

    SemanticVersion result;
    int *parts[] = {&result.major, &result.minor, &result.patch};
    for (std::size_t part = 0; part < 3; ++part) {
        if (position >= value.size() || value[position] < L'0' || value[position] > L'9') {
            return std::nullopt;
        }

        int number = 0;
        while (position < value.size() && value[position] >= L'0' && value[position] <= L'9') {
            const int digit = value[position] - L'0';
            if (number > (std::numeric_limits<int>::max() - digit) / 10) {
                return std::nullopt;
            }
            number = number * 10 + digit;
            ++position;
        }
        *parts[part] = number;

        if (part < 2) {
            if (position >= value.size() || value[position] != L'.') {
                return std::nullopt;
            }
            ++position;
        }
    }

    if (position != value.size()) {
        return std::nullopt;
    }
    return result;
}

bool IsNewer(const SemanticVersion &candidate, const SemanticVersion &current) {
    if (candidate.major != current.major) return candidate.major > current.major;
    if (candidate.minor != current.minor) return candidate.minor > current.minor;
    return candidate.patch > current.patch;
}

std::wstring WithoutLeadingV(std::wstring value) {
    if (!value.empty() && (value.front() == L'v' || value.front() == L'V')) {
        value.erase(value.begin());
    }
    return value;
}

[[noreturn]] void ThrowStopped() {
    throw boost::system::system_error(asio::error::operation_aborted);
}

[[noreturn]] void ThrowTimedOut() {
    throw boost::system::system_error(asio::error::timed_out);
}

[[noreturn]] void ThrowLastError() {
    throw boost::system::system_error(
        static_cast<int>(::GetLastError()),
        boost::system::system_category()
    );
}

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept { ::CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct AddressListFreer {
    void operator()(ADDRINFOEXW *addresses) const noexcept { ::FreeAddrInfoExW(addresses); }
};

UniqueHandle CreateManualResetEvent() {
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        ThrowLastError();
    }
    return UniqueHandle(event);
}

DWORD MillisecondsUntil(Deadline deadline) {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return remaining.count() > 0 ? static_cast<DWORD>(remaining.count()) : 0;
}

// asio's resolver runs getaddrinfo on a private thread that ~io_context joins, so a
// stalled lookup could not be abandoned. Overlapped GetAddrInfoExW can be cancelled.
std::vector<tcp::endpoint> ResolveGitHub(const std::stop_token &stop, Deadline deadline) {
    const UniqueHandle completed = CreateManualResetEvent();
    const UniqueHandle stop_requested = CreateManualResetEvent();
    // Runs on the thread that calls Stop(); it only wakes the wait below.
    const std::stop_callback on_stop(stop, [event = stop_requested.get()] {
        ::SetEvent(event);
    });

    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    OVERLAPPED overlapped{};
    overlapped.hEvent = completed.get();
    ADDRINFOEXW *addresses = nullptr;
    HANDLE lookup = nullptr;

    int status = ::GetAddrInfoExW(kGitHubHostWide, kGitHubPort, NS_DNS, nullptr, &hints,
        &addresses, nullptr, &overlapped, nullptr, &lookup);
    bool abandoned = false;
    if (status == WSA_IO_PENDING) {
        const HANDLE events[] = {completed.get(), stop_requested.get()};
        const DWORD wait = ::WaitForMultipleObjects(
            2, events, FALSE, MillisecondsUntil(deadline));
        if (wait != WAIT_OBJECT_0) {
            abandoned = true;
            ::GetAddrInfoExCancel(&lookup);
            // The lookup owns `overlapped` and `addresses` until it signals, even
            // when cancelled, so both must outlive this wait.
            ::WaitForSingleObject(completed.get(), INFINITE);
        }
        status = ::GetAddrInfoExOverlappedResult(&overlapped);
    }
    const std::unique_ptr<ADDRINFOEXW, AddressListFreer> owned_addresses(addresses);

    if (abandoned) {
        if (stop.stop_requested()) {
            ThrowStopped();
        }
        ThrowTimedOut();
    }
    if (status != 0) {
        throw boost::system::system_error(status, boost::system::system_category());
    }

    std::vector<tcp::endpoint> endpoints;
    for (const ADDRINFOEXW *address = addresses; address != nullptr; address = address->ai_next) {
        if ((address->ai_family != AF_INET && address->ai_family != AF_INET6) ||
            address->ai_addr == nullptr) {
            continue;
        }
        tcp::endpoint endpoint;
        if (address->ai_addrlen > endpoint.capacity()) {
            continue;
        }
        std::memcpy(endpoint.data(), address->ai_addr, address->ai_addrlen);
        endpoint.resize(address->ai_addrlen);
        endpoints.push_back(endpoint);
    }
    if (endpoints.empty()) {
        throw boost::system::system_error(asio::error::host_not_found);
    }
    return endpoints;
}

// Every step runs on the stream's io_context. The stop flag is checked right before
// each step starts; a stop that lands while a step is pending closes the socket
// instead (see DownloadLatestRelease), so between the two no step is left running.
asio::awaitable<HttpResponse> FetchLatestRelease(
    ssl::stream<beast::tcp_stream> &stream,
    const std::vector<tcp::endpoint> &endpoints,
    const std::stop_token &stop
) {
    if (stop.stop_requested()) ThrowStopped();
    co_await beast::get_lowest_layer(stream).async_connect(endpoints, asio::use_awaitable);

    if (stop.stop_requested()) ThrowStopped();
    co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);

    http::request<http::empty_body> request(http::verb::get, kGitHubLatestReleaseTarget, 11);
    request.set(http::field::host, kGitHubHost);
    request.set(http::field::accept, "application/vnd.github+json");
    request.set(http::field::user_agent,
        "maccy-for-windows/" + std::string(AppVersion::kStringUtf8));
    request.set("X-GitHub-Api-Version", "2022-11-28");

    if (stop.stop_requested()) ThrowStopped();
    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaximumResponseBytes);
    if (stop.stop_requested()) ThrowStopped();
    co_await http::async_read(stream, buffer, parser, asio::use_awaitable);

    // No TLS close_notify exchange: the parser has the complete, length-framed
    // response, and waiting for the peer's reply would only add a step that can stall.
    http::response<http::string_body> response = parser.release();
    co_return HttpResponse{response.result_int(), std::move(response.body())};
}

HttpResponse DownloadLatestRelease(const std::stop_token &stop) {
    const Deadline deadline = std::chrono::steady_clock::now() + kCheckTimeout;
    const std::vector<tcp::endpoint> endpoints = ResolveGitHub(stop, deadline);

    asio::io_context io_context;
    ssl::context tls_context(ssl::context::tls_client);
    // set_default_verify_paths() reads OpenSSL's OPENSSLDIR, which holds no CA
    // bundle on Windows, so every handshake failed verification. OpenSSL's winstore
    // loader trusts the Windows root store instead, as the rest of the system does.
    if (SSL_CTX_load_verify_store(tls_context.native_handle(), "org.openssl.winstore:") != 1) {
        throw std::runtime_error("Unable to load the Windows root certificates");
    }
    tls_context.set_verify_mode(ssl::verify_peer);

    ssl::stream<beast::tcp_stream> stream(io_context, tls_context);
    if (SSL_set_tlsext_host_name(stream.native_handle(), kGitHubHost) == 0) {
        throw std::runtime_error("Unable to configure the TLS server name");
    }
    // verify_peer alone accepts any publicly trusted certificate for any domain;
    // this makes OpenSSL also require that the certificate names api.github.com.
    if (SSL_set1_host(stream.native_handle(), kGitHubHost) != 1) {
        throw std::runtime_error("Unable to configure TLS host name verification");
    }
    // Beast keeps this absolute expiry for every later operation on the stream, so
    // connect, handshake, write and read all share the one deadline.
    beast::get_lowest_layer(stream).expires_at(deadline);

    std::optional<HttpResponse> response;
    std::exception_ptr failure;
    asio::co_spawn(io_context, FetchLatestRelease(stream, endpoints, stop),
        [&](std::exception_ptr error, HttpResponse result) {
            failure = error;
            if (!error) {
                response = std::move(result);
            }
        });

    // Runs on the thread that calls Stop(), which must not touch the socket; the
    // close is posted to this thread and makes the pending step fail at once.
    const std::stop_callback on_stop(stop, [&io_context, &stream] {
        asio::post(io_context, [&stream] { beast::get_lowest_layer(stream).close(); });
    });
    io_context.run();

    if (failure) {
        std::rethrow_exception(failure);
    }
    if (!response) {
        throw std::runtime_error("The update request ended without a response");
    }
    return std::move(*response);
}

std::optional<std::string> JsonString(const json::object &object, const char *key) {
    const json::value *value = object.if_contains(key);
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    const json::string &string = value->as_string();
    return std::string(string.data(), string.size());
}

json::object DownloadAndParseLatestRelease(const std::stop_token &stop) {
    const HttpResponse response = DownloadLatestRelease(stop);
    boost::system::error_code parse_error;
    const json::value document = json::parse(
        json::string_view(response.body.data(), response.body.size()),
        parse_error
    );

    if (response.status != 200) {
        if (!parse_error && document.is_object()) {
            const auto message = JsonString(document.as_object(), "message");
            if (message.has_value()) {
                throw LocalizedUpdateError(Localization::Format(
                    IDS_UPDATE_HTTP_ERROR,
                    {std::to_wstring(response.status), Localization::FromUtf8(*message)}
                ));
            }
        }
        throw LocalizedUpdateError(Localization::Format(
            IDS_UPDATE_HTTP_STATUS,
            {std::to_wstring(response.status)}
        ));
    }

    if (parse_error) {
        throw LocalizedUpdateError(Localization::Text(IDS_UPDATE_INVALID_JSON));
    }

    if (!document.is_object()) {
        throw LocalizedUpdateError(Localization::Text(IDS_UPDATE_INVALID_RELEASE));
    }
    return document.as_object();
}

} // namespace

bool IsNewerSemanticVersion(std::wstring_view current, std::wstring_view candidate) {
    const auto current_version = ParseSemanticVersion(current);
    const auto candidate_version = ParseSemanticVersion(candidate);
    return current_version.has_value() && candidate_version.has_value() &&
        IsNewer(*candidate_version, *current_version);
}

bool IsExpectedReleaseUrl(std::string_view url) {
    // The URL comes from the network and is opened in the browser, so it must be
    // exactly prefix + one tag segment. Matching the prefix byte for byte rules out
    // other schemes, hosts, ports and userinfo; restricting the tag's characters
    // rules out "/", "\", "%", "?" and "#", so no path traversal or encoded variant
    // can leave the repository's releases. A leading alphanumeric excludes "." and "..".
    if (!url.starts_with(kGitHubReleaseTagUrlPrefix)) {
        return false;
    }
    const std::string_view tag = url.substr(kGitHubReleaseTagUrlPrefix.size());
    const auto is_alphanumeric = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    return !tag.empty() && is_alphanumeric(tag.front()) &&
        std::ranges::all_of(tag, [&](char c) {
            return is_alphanumeric(c) || c == '.' || c == '-' || c == '_';
        });
}

UpdateChecker::~UpdateChecker() {
    Stop();
}

void UpdateChecker::SetWindow(HWND window) noexcept {
    std::lock_guard lock(m_mutex);
    m_window = window;
}

bool UpdateChecker::Start(UpdateCheckMode mode) {
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping || m_checking) {
            return false;
        }
        // Reap a finished worker; never join while m_checking (L-05).
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_checking = true;
    }

    try {
        m_worker = std::jthread([this, mode](std::stop_token stop) { Run(stop, mode); });
    } catch (...) {
        std::lock_guard lock(m_mutex);
        m_checking = false;
        return false;
    }
    return true;
}

bool UpdateChecker::IsChecking() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_checking;
}

std::vector<UpdateCheckResult> UpdateChecker::TakeResults() {
    std::vector<UpdateCheckResult> results;
    std::lock_guard lock(m_mutex);
    results.swap(m_results);
    return results;
}

void UpdateChecker::Stop() noexcept {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_window = nullptr;
    }
    // Exit runs this on the UI thread, so the join must not wait out the network:
    // the stop request aborts the worker's pending DNS lookup or socket step, which
    // makes the join return promptly.
    m_worker.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    std::lock_guard lock(m_mutex);
    m_checking = false;
    m_results.clear();
}

void UpdateChecker::Run(const std::stop_token &stop, UpdateCheckMode mode) {
    UpdateCheckResult result = Check(stop, mode);
    HWND window = nullptr;
    {
        std::lock_guard lock(m_mutex);
        m_checking = false;
        if (!m_stopping) {
            m_results.push_back(std::move(result));
            window = m_window;
        }
    }
    if (window != nullptr) {
        ::PostMessageW(window, AppConstants::kUpdateCheckerResultMessage, 0, 0);
    }
}

UpdateCheckResult UpdateChecker::Check(const std::stop_token &stop, UpdateCheckMode mode) {
    UpdateCheckResult result;
    result.mode = mode;
    result.current_version = AppConstants::kAppVersion;

    try {
        const auto current = ParseSemanticVersion(result.current_version);
        if (!current) {
            throw LocalizedUpdateError(Localization::Text(IDS_UPDATE_CURRENT_VERSION_INVALID));
        }

        const json::object release = DownloadAndParseLatestRelease(stop);
        const auto tag = JsonString(release, "tag_name");
        if (!tag.has_value() || tag->empty()) {
            throw LocalizedUpdateError(Localization::Text(IDS_UPDATE_RELEASE_TAG_MISSING));
        }
        result.latest_version = WithoutLeadingV(Localization::FromUtf8(*tag));
        const auto latest = ParseSemanticVersion(result.latest_version);
        if (!latest) {
            throw LocalizedUpdateError(Localization::Text(IDS_UPDATE_RELEASE_VERSION_INVALID));
        }

        const auto url = JsonString(release, "html_url");
        if (url.has_value() && IsExpectedReleaseUrl(*url)) {
            result.release_url = Localization::FromUtf8(*url);
        }
        if (result.release_url.empty()) {
            result.release_url = kGitHubReleasePage;
        }

        result.succeeded = true;
        result.update_available = IsNewerSemanticVersion(result.current_version, result.latest_version);
    } catch (const LocalizedUpdateError &error) {
        result.error = error.Message();
    } catch (const boost::system::system_error &error) {
        // what() appends the throwing header's path and function signature, which
        // means nothing to the user; the code's message is the readable part.
        result.error = Localization::Format(
            IDS_UPDATE_NETWORK_ERROR,
            {Localization::FromUtf8(error.code().message())}
        );
    } catch (const std::exception &error) {
        result.error = Localization::Format(
            IDS_UPDATE_NETWORK_ERROR,
            {Localization::FromUtf8(error.what())}
        );
    } catch (...) {
        result.error = Localization::Text(IDS_UPDATE_UNKNOWN_ERROR);
    }
    return result;
}
