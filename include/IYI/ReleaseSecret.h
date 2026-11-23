// =============================================================================
// include/IYI/ReleaseSecret.h — Release Decryption Secret Provider (Placeholder)
// =============================================================================
//
// CONTRACT SPECIFICATION FOR RELEASE SECRET PROVIDER
// -----------------------------------------------------------------------------
// This header is a committed repository placeholder providing an empty secret.
// It is intended for use ONLY when IYI_SHIPPING_BUILD is defined.
//
// In shipping distribution builds, the release packaging pipeline generates an
// authentic replacement ReleaseSecret.h header into a build-time include directory
// that takes precedence in the compiler's include path (e.g. via -I or CMake
// target_include_directories prepending the build include directory before include/).
//
// CONTRACT REQUIREMENTS FOR GENERATED ReleaseSecret.h HEADERS:
// -----------------------------------------------------------------------------
// 1. Never store the decryption key/passphrase as a single contiguous plaintext
//    string literal anywhere in the file. String literals embedded in binaries
//    are easily extracted via simple tools (strings, grep).
// 2. The secret MUST be partitioned into multiple pieces or byte fragments
//    (e.g., static const char arrays or XOR-masked byte arrays) at compile time.
// 3. At runtime, when ReleaseSecret is instantiated, its constructor or Reassemble()
//    method must assemble the pieces into an internal buffer.
// 4. The internal buffer MUST be zero-wiped (using volatile writes or a secure wipe
//    routine) immediately upon destruction or when Wipe() is invoked.
// 5. The class must reside in namespace IYI and provide the following public API:
//
//      namespace IYI {
//      class ReleaseSecret {
//      public:
//          ReleaseSecret();                        // Reassembles fragments into buffer
//          ~ReleaseSecret();                       // Calls Wipe() to erase plaintext
//          ReleaseSecret(const ReleaseSecret&) = delete;
//          ReleaseSecret& operator=(const ReleaseSecret&) = delete;
//          ReleaseSecret(ReleaseSecret&&) noexcept;
//          ReleaseSecret& operator=(ReleaseSecret&&) noexcept;
//
//          const char* Get() const;                // Returns pointer to null-terminated string
//          size_t Size() const;                    // Key length in bytes (excluding '\0')
//          bool IsEmpty() const;                   // Returns true if Size() == 0
//          void Wipe();                            // Zeroes out buffer memory and clears it
//      };
//      }
//
// 6. Public header cleanliness invariant: No Python.h, pybind11/*, nlohmann/json.hpp,
//    or rlgl.h may be included in this header.
// =============================================================================

#pragma once

#include <cstddef>
#include <vector>

namespace IYI {

class ReleaseSecret
{
public:
    ReleaseSecret() = default;   // placeholder: empty secret

    ~ReleaseSecret()
    {
        Wipe();
    }

    ReleaseSecret(const ReleaseSecret&) = delete;
    ReleaseSecret& operator=(const ReleaseSecret&) = delete;

    ReleaseSecret(ReleaseSecret&& other) noexcept
        : m_buffer(std::move(other.m_buffer))
    {
    }

    ReleaseSecret& operator=(ReleaseSecret&& other) noexcept
    {
        if (this != &other) {
            Wipe();
            m_buffer = std::move(other.m_buffer);
        }
        return *this;
    }

    // Returns a pointer to the reassembled null-terminated secret string.
    const char* Get() const
    {
        return m_buffer.empty() ? "" : m_buffer.data();
    }

    // Returns the number of characters in the secret (excluding the null terminator).
    size_t Size() const
    {
        return m_buffer.empty() ? 0 : (m_buffer.size() - 1);
    }

    // Returns true if the secret is empty (placeholder default).
    bool IsEmpty() const
    {
        return Size() == 0;
    }

    // Securely wipes the reassembled secret from memory using volatile writes.
    void Wipe()
    {
        volatile char* p = m_buffer.data();
        for (size_t i = 0; i < m_buffer.size(); ++i) {
            p[i] = 0;
        }
        m_buffer.clear();
    }

private:
    std::vector<char> m_buffer;
};

} // namespace IYI
