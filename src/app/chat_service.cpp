#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sodium.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "app/chat_service.hpp"
#include "crypto/psk_aead.hpp"
#include "proto/ctrl.hpp"
#include "proto/frag.hpp"
#include "transport/itransport.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

namespace app
{

constexpr std::uint8_t AAD[] = {'B', 'C', '1'};
// HKDF expand contexts
static constexpr const char CTX_KE_C2P[] = "bcKC2P1";
static constexpr const char CTX_KE_P2C[] = "bcKP2C1";
static constexpr const char CTX_N_C2P[]  = "bcNC2P1";
static constexpr const char CTX_N_P2C[]  = "bcNP2C1";

inline bool get_na32(std::array<uint8_t, 32> &out)
{
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    size_t got = 0;
    while (got < out.size())
    {
        ssize_t n = ::read(fd, out.data() + got, out.size() - got);
        if (n <= 0)
        {
            ::close(fd);
            return false;
        }
        got += size_t(n);
    }
    ::close(fd);
    return true;
}

ChatService::ChatService(transport::ITransport &t, aead::PskAead &aead, std::size_t mtu_payload)
    : tx_(t), aead_(aead), mtu_payload_(mtu_payload)
{
}

bool ChatService::start()
{
    // in case there are any background hello-threads
    stop();

    // Default to loopback
    // If BITCHAT_TRANSPORT=bluez we pass BLE UUIDs and role
    auto env_or = [](const char *key, const char *defv) -> std::string {
        const char *v = std::getenv(key);
        return v ? std::string(v) : std::string(defv);
    };

    const std::string which = env_or("BITCHAT_TRANSPORT", "loopback");

    transport::Settings s{};
    s.mtu_payload = mtu_payload_;

    if (which == "bluez")
    {
        // Role for BlueZ: "central" or "peripheral" (default peripheral)
        s.role = env_or("BITCHAT_ROLE", "peripheral");
        // Fixed UUIDs per project spec.
        s.svc_uuid = constants::SVC_UUID;
        s.tx_uuid  = constants::TX_UUID;  // Notify
        s.rx_uuid  = constants::RX_UUID;  // Write w/ response
    }
    else
    {
        // Loopback for testing/dev
        s.role = "loopback";
    }

    bool ok = tx_.start(s, [this](const transport::Frame &f) { this->on_rx(f); });
    if (!ok)
        return false;

    is_central_ = (s.role == "central");
    // Decide local capaibility bit
    std::vector<uint8_t> tmp_psk;
    local_has_psk_ = parse_psk_env(std::getenv("BITCHAT_PSK"), tmp_psk) && !tmp_psk.empty();
    if (!tmp_psk.empty())
        sodium_memzero(tmp_psk.data(), tmp_psk.size());
    local_caps_ = local_has_psk_ ? ctrl::CAP_AEAD_PSK_SUPPORTED : 0;

    // Decide if we run HELLO thread (default: only for bluez, or env override)
    const char *ctrl_env     = std::getenv("BITCHAT_CTRL_HELLO");
    const bool  enable_hello = (ctrl_env ? (std::strcmp(ctrl_env, "0") != 0)
                                         : (tx_.name() == std::string("bluez")));
    ctrl_hello_enabled_      = enable_hello;

    if (!enable_hello)
    {
        return true;  // no hello packet for loopback
    }

    // Prepare user ID (may be empty)
    if (const char *u = std::getenv("BITCHAT_USER_ID"))
        local_user_ = u;
    else
        local_user_.clear();
    if (local_user_.size() > 64)
        local_user_.resize(64);

    hello_stop_.store(false);
    hello_sent_ = false;
    get_na32(na32_);
    have_na_local_ = local_has_psk_;
    // clear old session when starting a new connection
    aead_.set_session(nullptr);
    aead_on_ = false;

    // thread to send hello packets
    hello_thr_ = std::thread([this] {
        bool last_ready = false;
        while (!hello_stop_.load())
        {
            const bool ready = tx_.link_ready();

            // new link edge: refresh Na32 & clear sent flag
            if (ready && !last_ready)
            {
                get_na32(na32_);
                have_na_local_ = local_has_psk_;
                have_na_peer_  = false;
                aead_.set_session(nullptr);
                aead_on_    = false;
                hello_sent_ = false;  // resend HELLO with fresh Na
            }

            if (ready && !hello_sent_)
            {
                // include T_NA32 only when local PSK is present
                auto bytes = ctrl::encode_hello(local_user_, local_caps_,
                                                local_has_psk_ ? na32_.data() : nullptr);
                if (tx_.send(bytes))
                {
                    hello_sent_ = true;
                    if (local_has_psk_)
                    {
                        LOG_INFO("[CTRL] HELLO out: user='%s' caps=0x%08x na32=%02x%02x...",
                                 local_user_.c_str(), local_caps_, (unsigned)na32_[0],
                                 (unsigned)na32_[1]);
                    }
                    else
                    {
                        LOG_INFO("[CTRL] HELLO out: user='%s' caps=0x%08x na32=(none)",
                                 local_user_.c_str(), local_caps_);
                    }
                }
            }
            if (!ready)
            {
                // Link down: clear session immediately
                hello_sent_ = false;
                aead_.set_session(nullptr);
                aead_on_ = false;
            }
            last_ready = ready;

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    return true;
}

void ChatService::stop()
{
    hello_stop_.store(true);
    if (hello_thr_.joinable())
        hello_thr_.join();
    tx_.stop();
}

bool ChatService::send_text(std::string_view msg)
{
    // 1) Encrypt
    std::vector<std::uint8_t> sealed_text;
    if (!aead_.seal(msg, AAD, sizeof(AAD), sealed_text))
    {
        LOG_ERROR("send_text: AEAD seal failed");
        return false;
    }

    // 2) Make chunk
    const std::size_t frame_mtu   = mtu_payload_;
    const std::size_t payload_mtu = (frame_mtu > frag::HDR_SIZE) ? (frame_mtu - frag::HDR_SIZE)
                                                                 : 0;
    auto              chunks = frag::make_chunks(next_id_.fetch_add(1), sealed_text, payload_mtu);
    if (!sealed_text.empty() && chunks.empty())
    {
        LOG_ERROR("send_text: make_chunks failed");
        return false;
    }

    // 3) Send
    for (const auto &ch : chunks)
    {
        auto frame = frag::serialize(ch);
        if (frame.empty() && ch.hdr.len != 0)
        {
            LOG_ERROR("send_text: serialize failed");
            return false;
        }
        if (!tx_.send(frame))
        {
            LOG_ERROR("send_text: transport.send failed");
            return false;
        }
    }
    return true;
}

void ChatService::on_rx(const transport::Frame &f)
{
    // CTRL_HELLO
    if (ctrl_hello_enabled_ && f.size() >= 2 && f[0] == ctrl::MSG_CTRL_HELLO &&
        f[1] == ctrl::HELLO_VER)
    {
        ctrl::Hello h{};
        if (ctrl::parse_hello(f.data(), f.size(), h))
        {
            if (!h.user_id.empty())
                peer_user_ = h.user_id;
            if (h.has_caps)
                peer_caps_ = h.caps;
            peer_has_psk_ = h.has_caps && (h.caps & ctrl::CAP_AEAD_PSK_SUPPORTED);
            if (h.has_na32)  // we know that peer has psk enabled
            {
                std::copy_n(h.na32.data(), 32, peer_na32_.data());
                have_na_peer_ = true;
            }
            else
            {
                std::fill(peer_na32_.begin(), peer_na32_.end(), 0);
            }
            maybe_kex();
            if (h.has_na32)
            {
                LOG_INFO("[CTRL] HELLO in: user='%s' caps=0x%08x na32=%02x%02x...",
                         (peer_user_.empty() ? "<none>" : peer_user_.c_str()), peer_caps_,
                         (unsigned)peer_na32_[0], (unsigned)peer_na32_[1]);
            }
            else
            {
                LOG_INFO("[CTRL] HELLO in: user='%s' caps=0x%08x na32=(none)",
                         (peer_user_.empty() ? "<none>" : peer_user_.c_str()), peer_caps_);
            }

            return;  // successfully parsed hello packet
        }
        // fallthrough if failed to parse hello packet
    }

    // parse -> reassemble -> aead.open -> print
    auto c = frag::parse(f);
    if (!c)
    {
        LOG_WARN("on_rx: dropping invalid frame");
        return;
    }
    // Feed to reassembler
    auto full = rx_.feed(*c);
    if (!full)  // not complete yet
        return;

    std::vector<std::uint8_t> plain;
    if (!aead_.open(*full, AAD, sizeof(AAD), plain))
    {
        LOG_WARN("[SEC] AEAD decrypt failed (PSK mismatch?) — dropping frame");
        return;
    }

    if (tail_enabled_.load(std::memory_order_relaxed))
    {
        LOG_INFO("[RECV] %.*s", (int)plain.size(), (const char *)plain.data());
        return;
    }
}

void ChatService::maybe_kex()
{
    if (!local_has_psk_ || !peer_has_psk_)
        return;
    if (!have_na_local_ || !have_na_peer_)
        return;
    if (aead_on_)
        return;
    derive_and_install();
}

inline bool is_hex_str(const std::string &s)
{
    if (s.size() % 2)
        return false;
    for (unsigned char c : s)
    {
        if (!std::isxdigit(c))
            return false;
    }
    return true;
}

bool ChatService::parse_psk_env(const char *env, std::vector<uint8_t> &out)
{
    // init out in case it is not empty
    out.clear();
    if (!env || !*env)
        return false;
    std::string s(env);

    // trim start/end spaces
    const auto whitespace = " \t";
    const auto s_start    = s.find_first_not_of(whitespace);
    const auto s_end      = s.find_last_not_of(whitespace);
    const auto s_range    = s_end - s_start + 1;
    s                     = s.substr(s_start, s_range);

    // read hex val and stor it into 'out' arr
    if (is_hex_str(s))
    {
        out.reserve(s.size() / 2);
        for (size_t j = 0; j < s.size(); j += 2)
        {
            unsigned v = 0;
            for (int k = 0; k < 2; k++)
            {
                char c = s[j + k];
                v <<= 4;
                if (c >= '0' && c <= '9')
                    v |= (c - '0');
                else if (c >= 'a' && c <= 'f')
                    v |= 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F')
                    v |= 10 + (c - 'A');
            }
            out.push_back((uint8_t)v);
        }
        return true;
    }

    // is a base64, using libsodium built-int to convert
    size_t maxlen = s.size() / 4 * 3 + 3;
    out.resize(maxlen);
    size_t real_len = 0;
    if (sodium_base642bin(out.data(), out.size(), s.c_str(), s.size(), nullptr, &real_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
    {
        out.clear();
        return false;
    }
    out.resize(real_len);
    return true;
}

void ChatService::derive_and_install()
{
    // HKDF: salt = PSK, IKM = Na||Nb（central||peripheral)
    std::array<uint8_t, 64> ikm{};
    if (is_central_)
    {
        // central || peripheral
        std::memcpy(ikm.data(), na32_.data(), 32);
        std::memcpy(ikm.data() + 32, peer_na32_.data(), 32);
    }
    else
    {
        // peripheral || central
        std::memcpy(ikm.data(), peer_na32_.data(), 32);
        std::memcpy(ikm.data() + 32, na32_.data(), 32);
    }

    // Using PSK as HKDF salt, IKM = Na||Nb with role-defined order
    std::vector<uint8_t> psk;
    if (!parse_psk_env(std::getenv("BITCHAT_PSK"), psk) || psk.empty())
    {
        LOG_WARN("[KEX] no/invalid PSK; aborting");
        return;
    }

    // HKDF extract
    std::array<uint8_t, 32> prk{};
    if (crypto_kdf_hkdf_sha256_extract(prk.data(), psk.empty() ? nullptr : psk.data(), psk.size(),
                                       ikm.data(), ikm.size()) != 0)
    {
        LOG_WARN("[KEX] HKDF-Extract failed");
        sodium_memzero(ikm.data(), ikm.size());
        return;
    }

    // deriving keys from prk (hkdf expand)
    aead::SessionKeys keys{};
    if (crypto_kdf_hkdf_sha256_expand(keys.ke_c2p.data(), keys.ke_c2p.size(), CTX_KE_C2P,
                                      sizeof(CTX_KE_C2P) - 1, prk.data()) != 0 ||
        crypto_kdf_hkdf_sha256_expand(keys.ke_p2c.data(), keys.ke_p2c.size(), CTX_KE_P2C,
                                      sizeof(CTX_KE_P2C) - 1, prk.data()) != 0 ||
        crypto_kdf_hkdf_sha256_expand(keys.n24_c2p.data(), keys.n24_c2p.size(), CTX_N_C2P,
                                      sizeof(CTX_N_C2P) - 1, prk.data()) != 0 ||
        crypto_kdf_hkdf_sha256_expand(keys.n24_p2c.data(), keys.n24_p2c.size(), CTX_N_P2C,
                                      sizeof(CTX_N_P2C) - 1, prk.data()) != 0)
    {
        LOG_WARN("[KEX] HKDF-Expand failed");
        sodium_memzero(prk.data(), prk.size());
        sodium_memzero(ikm.data(), ikm.size());
        sodium_memzero(keys.ke_c2p.data(), keys.ke_c2p.size());
        sodium_memzero(keys.ke_p2c.data(), keys.ke_p2c.size());
        sodium_memzero(keys.n24_c2p.data(), keys.n24_c2p.size());
        sodium_memzero(keys.n24_p2c.data(), keys.n24_p2c.size());
        return;
    }

    aead::SessionKeys k_local = keys;
    if (!is_central_)
    {
        std::swap(k_local.ke_c2p, k_local.ke_p2c);
        std::swap(k_local.n24_c2p, k_local.n24_p2c);
    }

    // Install and start session
    if (aead_.set_session(&k_local))
    {
        aead_on_ = true;
        LOG_INFO("[KEX] complete. AEAD is now enabled");
    }
    else
    {
        LOG_WARN("[KEX] install failed. Staying plaintext");
    }

    sodium_memzero(prk.data(), prk.size());
    sodium_memzero(ikm.data(), ikm.size());
    if (!psk.empty())
        sodium_memzero(psk.data(), psk.size());
}

}  // namespace app
