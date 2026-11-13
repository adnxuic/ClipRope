#include <windows.h>
#include <ole2.h>

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--self-test") {
        std::cerr << "Usage: cliprope_probe --self-test\n";
        return 2;
    }
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        std::cerr << "The probe requires Windows x64.\n";
        return 1;
    }
    if (FAILED(OleInitialize(nullptr))) {
        std::cerr << "Could not initialize the OLE STA.\n";
        return 1;
    }
    HWND listener = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, nullptr, nullptr);
    const bool registered = listener && AddClipboardFormatListener(listener);
    if (registered) RemoveClipboardFormatListener(listener);
    if (listener) DestroyWindow(listener);
    OleUninitialize();
    if (!registered) {
        std::cerr << "Could not register the message-only clipboard listener.\n";
        return 1;
    }
    std::cout << "x64 OLE STA and clipboard listener ready\n";
    return 0;
}
