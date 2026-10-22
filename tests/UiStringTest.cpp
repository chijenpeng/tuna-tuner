// UI strings are plain const char* literals handed to juce::String (Latin-1): a non-ASCII byte shows up as mojibake
// ("Â·"). Fail if any appears in the editor or processor sources.
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef SOURCE_ROOT
#define SOURCE_ROOT "."
#endif

int main() {
    int bad = 0;
    for (const char* name : { "Source/PluginEditor.cpp", "Source/PluginEditor.h", "Source/PluginProcessor.cpp" }) {
        std::ifstream f(std::string(SOURCE_ROOT) + "/" + name, std::ios::binary);
        if (!f) { std::cerr << "cannot read " << name << '\n'; return 1; }
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        int line = 1;
        for (const unsigned char c : text) {
            if (c == '\n') ++line;
            if (c >= 0x80) { std::cerr << name << ":" << line << " contains a non-ASCII byte\n"; ++bad; break; }
        }
    }
    std::cout << (bad ? "UI string test FAILED\n" : "UI strings are ASCII\n");
    return bad ? 1 : 0;
}
