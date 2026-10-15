#pragma once
#include <array>
#include <cstdint>

namespace rdr2::diagnostic {
// The caller serializes this state and applies decisions only at a drained
// original command-buffer Begin. This class never changes worker state.
class WorkerModeControl final {
public:
    struct Decision final {
        unsigned mode{1};
        std::uint64_t nonce{}, deadlineMs{};
        bool leased{};
    };
private:
    std::array<std::uint64_t,4> key_{};
    std::uint64_t largestNonce_{}, deadline_{}, lastMs_{};
    bool haveKey_{}, retired_{};

    static bool parse(const char* text, std::array<std::uint64_t,4>& values) noexcept {
        if (!text || !*text) return false;
        constexpr std::array<std::uint64_t,4> limits{1, UINT32_MAX, UINT64_MAX, 10000};
        unsigned field{}, length{};
        bool digit{};
        for (const char* p=text; *p; ++p) {
            if (++length>91) return false;
            if (*p==':') {
                if (!digit || field==3) return false;
                ++field; digit=false; continue;
            }
            if (*p<'0' || *p>'9') return false;
            const auto next=unsigned(*p-'0');
            if (values[field]>limits[field]/10 ||
                (values[field]==limits[field]/10 && next>limits[field]%10)) return false;
            values[field]=values[field]*10+next; digit=true;
        }
        return field==3 && digit;
    }
public:
    // Format: mode:pid:nonce:ttlMs. Nonces increase within one process.
    // Repeating a property never renews its first accepted deadline.
    Decision sample(unsigned pid, std::uint64_t ms, const char* property) noexcept {
        std::array<std::uint64_t,4> command{};
        if (!parse(property,command) || !pid || command[1]!=pid || !command[2] ||
            command[3]<1000 || !ms || ms>UINT64_MAX-command[3]) {
            if (haveKey_) retired_=true;
            return {};
        }
        if (command[2]<largestNonce_) return {};
        if (command[2]==largestNonce_) {
            if (!haveKey_ || key_!=command) { retired_=true; return {}; }
        } else {
            largestNonce_=command[2]; key_=command; haveKey_=true;
            deadline_=ms+command[3]; lastMs_=ms; retired_=false;
        }
        if (ms<lastMs_ || ms>=deadline_) retired_=true;
        lastMs_=ms;
        if (retired_) return {};
        return {unsigned(command[0]), command[2], deadline_, true};
    }
};
}
