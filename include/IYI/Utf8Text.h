// UTF-8 helpers for UITextInput: codepoint encoding, classification and single-codepoint backspace.
// Plain std-only inline functions so they can be unit tested without a window.
// Scope: no shaping, no RTL, no grapheme clusters; one codepoint is one editing unit.

#pragma once

#include <cstddef>
#include <string>

namespace IYI {

enum class CharClass { Reject, Letter, Digit, Symbol };

inline CharClass ClassifyCodepoint(int cp)
{
    if (cp < 32 || cp == 127 || (cp >= 0x80 && cp <= 0x9F)) return CharClass::Reject;   // controls
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return CharClass::Reject;       // surrogates / out of range
    if (cp >= '0' && cp <= '9') return CharClass::Digit;
    if (cp < 0x80) return ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) ? CharClass::Letter : CharClass::Symbol;
    // Hand-rolled letter ranges for Latin, Greek, Cyrillic, Han/kana and Hangul;
    // everything else printable counts as Symbol. Upgrade to a real Unicode category table if more scripts ship.
    const bool letter =
        cp == 0xAA || cp == 0xB5 || cp == 0xBA ||
        (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) ||   // Latin-1 + Latin Extended-A/B
        (cp >= 0x370 && cp <= 0x3FF) ||                              // Greek
        (cp >= 0x400 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x52F) || // Cyrillic
        (cp >= 0x1E00 && cp <= 0x1EFF) ||                            // Latin Extended Additional
        (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) || // kana
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || // Han
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x323AF) ||
        (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3131 && cp <= 0x318E) || // Hangul
        (cp >= 0xAC00 && cp <= 0xD7A3);
    return letter ? CharClass::Letter : CharClass::Symbol;
}

// A TextInput's Allow* rules: whether a typed or pasted codepoint is kept.
inline bool AcceptCodepoint(int cp, bool letters, bool digits, bool symbols)
{
    const CharClass cls = ClassifyCodepoint(cp);
    return (cls == CharClass::Letter && letters) || (cls == CharClass::Digit && digits) || (cls == CharClass::Symbol && symbols);
}

inline void AppendUtf8(std::string& out, int cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Removes the last codepoint (all its continuation bytes). Returns false when already empty.
inline bool PopLastCodepoint(std::string& s)
{
    if (s.empty()) return false;
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back(); // the lead byte
    return true;
}

inline size_t CountCodepoints(const std::string& s)
{
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Keeps only the codepoints a TextInput would accept when typed (AcceptCodepoint), so newlines and other
// controls are dropped. Invalid UTF-8 bytes are skipped.
constexpr int kMaxPasteCodepoints = 1024;  // a huge clipboard must not stall the frame

inline std::string FilterPaste(const std::string& clip, bool letters, bool digits, bool symbols)
{
    std::string out;
    int kept = 0;
    for (size_t i = 0; i < clip.size() && kept < kMaxPasteCodepoints;) {
        const unsigned char b = static_cast<unsigned char>(clip[i]);
        const int n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        int cp = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
        bool ok = n > 0 && i + n <= clip.size();
        for (int k = 1; ok && k < n; ++k) {
            const unsigned char c = static_cast<unsigned char>(clip[i + k]);
            ok = (c & 0xC0) == 0x80;
            cp = (cp << 6) | (c & 0x3F);
        }
        i += ok ? n : 1;
        if (ok && AcceptCodepoint(cp, letters, digits, symbols)) {
            AppendUtf8(out, cp);
            ++kept;
        }
    }
    return out;
}

} // namespace IYI
