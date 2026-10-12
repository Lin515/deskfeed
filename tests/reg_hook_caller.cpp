// Test-only fixture: an IAT-rewrite target.
//
// It does exactly one thing: import RegOpenKeyExW / RegQueryValueExW / RegGetValueW by name (the way
// XAML does) and re-export three thin wrappers around them. deskfeed-native-tests LoadLibrary's it so
// that reg_hook can patch its import slots and the replacement path can be exercised for real
// (including "unload the module, then restore").
//
// Rules: links the SDK/advapi32 only, never any library of this project; never deployed, it is only a
// CTest fixture.
#include <windows.h>

extern "C" __declspec(dllexport) LONG WINAPI CallerRegOpen(HKEY root, LPCWSTR sub_key, REGSAM access,
                                                           PHKEY result) {
    return ::RegOpenKeyExW(root, sub_key, 0, access, result);
}

extern "C" __declspec(dllexport) LONG WINAPI CallerRegQuery(HKEY key, LPCWSTR value_name,
                                                            LPDWORD type, LPBYTE data, LPDWORD size) {
    return ::RegQueryValueExW(key, value_name, nullptr, type, data, size);
}

extern "C" __declspec(dllexport) LONG WINAPI CallerRegGet(HKEY root, LPCWSTR sub_key,
                                                          LPCWSTR value_name, DWORD flags,
                                                          LPDWORD type, PVOID data, LPDWORD size) {
    return ::RegGetValueW(root, sub_key, value_name, flags, type, data, size);
}

extern "C" __declspec(dllexport) LONG WINAPI CallerRegClose(HKEY key) { return ::RegCloseKey(key); }

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
