#include "text.h"

#include <cstdint>

namespace {
// Latin Extended-A (U+0100–U+017F) without its accents, from Unicode's decompositions (Œ → OE, ł → l…).
const char* const LATIN_EXTENDED_A[128] = {
    "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d", "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g",
    "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i", "I", "i", "IJ", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L",
    "l", "L", "l", "N", "n", "N", "n", "N", "n", "'n", "N", "n", "O", "o", "O", "o", "O", "o", "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s",
    "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u", "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s",
};

void Append(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// What the font shows instead of cp; nullptr keeps it as is. Never a double quote nor a backslash: ForFont also runs
// on whole JSON documents.
const char* Replacement(uint32_t cp) {
    if (cp >= 0x0100 && cp <= 0x017F) return LATIN_EXTENDED_A[cp - 0x0100];
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x2035: case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "'";
    case 0x2022: case 0x2023: case 0x2043: case 0x25CF: case 0x25E6: return "\xC2\xB7"; // ·
    case 0x2026: return "...";
    case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F: case 0x205F: return "\xC2\xA0"; // no-break space
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF: return "";
    case 0x2190: return "<-";
    case 0x2192: case 0x279C: case 0x27A1: return "->";
    case 0x2191: return "^";
    case 0x2193: return "v";
    case 0x2194: return "<->";
    case 0x21D2: return "=>";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x2260: return "!=";
    case 0x2248: return "~";
    case 0x2713: case 0x2714: case 0x2611: case 0x2705: return "OK";
    case 0x2717: case 0x2718: case 0x2612: case 0x274C: return "X";
    case 0x2605: case 0x2606: return "*";
    case 0x25B6: case 0x25BA: case 0x27A4: return "\xC2\xBB"; // »
    case 0x25C0: case 0x25C4: return "\xC2\xAB"; // «
    case 0x20AC: return "EUR";
    case 0x2016: return "||";
    case 0x2017: return "_";
    case 0x2020: return "+";
    case 0x2021: return "++";
    case 0x2024: return ".";
    case 0x2025: return "..";
    case 0x2027: return "\xC2\xB7";
    case 0x2028: case 0x2029: return " ";
    case 0x2030: return "o/oo";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x203C: return "!!";
    case 0x2044: return "/";
    case 0x2047: return "??";
    case 0x2048: return "?!";
    case 0x2049: return "!?";
    default: break;
    }
    // The other spaces of General Punctuation: a no-break space; its invisible marks and the rest: gone.
    if (cp >= 0x2000 && cp <= 0x200A) return "\xC2\xA0";
    if (cp >= 0x2000 && cp <= 0x206F) return "";
    // Symbols, dingbats and emoji the font cannot have: gone rather than "?".
    if ((cp >= 0x2190 && cp <= 0x2BFF) || (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0xFE00 && cp <= 0xFE0F)) return "";
    return nullptr;
}
} // namespace

std::string ForFont(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    const unsigned char* s = (const unsigned char*)utf8.data();
    const size_t size = utf8.size();
    for (size_t i = 0; i < size;) {
        unsigned char c = s[i];
        if (c < 0x80) { out += (char)c; i++; continue; }
        // One UTF-8 sequence; a broken one is copied byte by byte (ImGui shows it as it would have).
        uint32_t cp = 0;
        size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        bool valid = length > 1 && i + length <= size;
        if (valid) {
            cp = c & (0xFF >> (length + 1));
            for (size_t k = 1; k < length; k++) {
                if ((s[i + k] & 0xC0) != 0x80) { valid = false; break; }
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
        }
        if (!valid) { out += (char)c; i++; continue; }
        if (const char* replacement = Replacement(cp)) out += replacement;
        else Append(out, cp);
        i += length;
    }
    return out;
}
