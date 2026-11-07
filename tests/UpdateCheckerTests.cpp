#include "UpdateChecker.h"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::string_view kTagUrl =
    "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/";

bool CheckSemanticVersions() {
    return IsNewerSemanticVersion(L"1.0.0", L"1.0.1") &&
        IsNewerSemanticVersion(L"1.0.9", L"v1.0.10") &&
        IsNewerSemanticVersion(L"1.9.9", L"2.0.0") &&
        !IsNewerSemanticVersion(L"1.0.1", L"1.0.1") &&
        !IsNewerSemanticVersion(L"1.0.2", L"1.0.1") &&
        !IsNewerSemanticVersion(L"1.0", L"1.0.1") &&
        !IsNewerSemanticVersion(L"1.0.0", L"release-1.0.1");
}

bool CheckReleaseUrlGuard() {
    constexpr std::string_view accepted[] = {
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.4.3",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/1.4.3",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v2.0.0-rc_1",
    };
    constexpr std::string_view rejected[] = {
        // Path traversal out of the releases, plain and encoded.
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/../../../attacker/repo",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/../../attacker/repo/releases/tag/v1.0.0",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/%2e%2e%2f%2e%2e%2fattacker",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0%2F..%2F..%2Fattacker",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0/../../attacker",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0\\..\\..\\attacker",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/.",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/..",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/.hidden",
        // Missing or extra pieces after the tag.
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0/",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0?x=1",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0#top",
        "https://github.com/abd3lraouf-studios/maccy_for_windows/releases/latest",
        // Other hosts, ports, userinfo and schemes.
        "https://github.com.evil.example/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0",
        "https://evil.example/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0",
        "https://github.com@evil.example/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0",
        "https://user@github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0",
        "https://github.com:443/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0",
        "http://github.com/abd3lraouf-studios/maccy_for_windows/releases/tag/v1.0.0",
        // Other repositories, including the upstream this fork came from.
        "https://github.com/halifox/maccy_for_windows/releases/tag/v1.0.0",
        "https://github.com/abd3lraouf-studios/other_repo/releases/tag/v1.0.0",
        "",
    };

    for (const std::string_view url : accepted) {
        if (!IsExpectedReleaseUrl(url)) return false;
    }
    for (const std::string_view url : rejected) {
        if (IsExpectedReleaseUrl(url)) return false;
    }
    return IsExpectedReleaseUrl(std::string(kTagUrl) + "v1.0.0");
}

// Exit calls Stop() on the UI thread, so it must return promptly whatever the
// network is doing. Whether the check itself succeeds depends on the machine's
// network and is deliberately not asserted.
bool CheckStopIsPrompt(std::chrono::milliseconds delay_before_stop) {
    UpdateChecker checker;
    if (!checker.Start(UpdateCheckMode::Manual)) return false;
    std::this_thread::sleep_for(delay_before_stop);

    const auto started = std::chrono::steady_clock::now();
    checker.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    return elapsed < std::chrono::seconds(1) &&
        !checker.IsChecking() &&
        checker.TakeResults().empty() &&
        !checker.Start(UpdateCheckMode::Manual);
}

} // namespace

int main() {
    if (!CheckSemanticVersions()) return 1;
    if (!CheckReleaseUrlGuard()) return 2;
    if (!CheckStopIsPrompt(std::chrono::milliseconds(0))) return 3;
    if (!CheckStopIsPrompt(std::chrono::milliseconds(300))) return 4;
    return 0;
}
