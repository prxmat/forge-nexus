// Native check of ForFont (src/text.cpp): typographic French and symbols become what Nexus' Latin-1 font has.
//   clang++ -std=c++17 -Isrc tools/text-check.cpp src/text.cpp -o build/native/text-check && build/native/text-check
#include <cstdio>
#include <string>

#include "text.h"

static int failures = 0;
static void Same(const std::string& input, const std::string& expected) {
    std::string got = ForFont(input);
    bool ok = got == expected;
    std::printf("%s %s -> %s\n", ok ? "  ok  " : "  FAIL", input.c_str(), got.c_str());
    if (!ok) { std::printf("        expected %s\n", expected.c_str()); failures++; }
}

// Nothing above U+00FF may remain.
static bool Latin1(const std::string& text) {
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0xC4) return false; // Lead bytes of U+0100 and up.
    }
    return true;
}

int main() {
    Same("L’essentiel – les œufs arrivent…", "L'essentiel - les oeufs arrivent...");
    Same("Gorseval — « Bouge ! » → stack", "Gorseval - « Bouge ! » -> stack");
    Same("Kill ✓ · 3✝ 2↓", "Kill OK · 3 2v");
    Same("Cœur, Œil, Łukasz, Dvořák", "Coeur, OEil, Lukasz, Dvorák");
    Same("Prête : 10 h", "Prête : 10 h");
    Same("é à ç ù « » ° ×", "é à ç ù « » ° ×");
    Same("“guillemets” et • puce", "'guillemets' et · puce");
    Same("Boss 🐉 vaincu 🎉", "Boss  vaincu ");
    Same("a\xE2\x80\xAF!", "a\xC2\xA0!");
    Same("broken \xE2\x80 end", "broken \xE2\x80 end");
    // A whole JSON document stays valid: no replacement brings a quote or a backslash.
    Same("{\"label\":\"L’œuf “x”\"}", "{\"label\":\"L'oeuf 'x'\"}");
    std::string all;
    for (unsigned cp = 0x100; cp < 0x3000; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        std::string one;
        if (cp < 0x800) { one += (char)(0xC0 | (cp >> 6)); one += (char)(0x80 | (cp & 0x3F)); }
        else { one += (char)(0xE0 | (cp >> 12)); one += (char)(0x80 | ((cp >> 6) & 0x3F)); one += (char)(0x80 | (cp & 0x3F)); }
        std::string mapped = ForFont(one);
        if (mapped.find('"') != std::string::npos || mapped.find('\\') != std::string::npos) { std::printf("  FAIL U+%04X gives a quote or a backslash\n", cp); failures++; }
        if ((cp <= 0x17F || (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x2190 && cp <= 0x27BF)) && !Latin1(mapped)) { std::printf("  FAIL U+%04X stays out of the font\n", cp); failures++; }
    }
    std::printf(failures ? "%d FAILED\n" : "all good\n", failures);
    return failures ? 1 : 0;
}
