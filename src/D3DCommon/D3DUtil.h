#pragma once

#include <string>

#include <windows.h>
#include <comdef.h>

class DxException {
  public:
    DxException() = default;
    DxException(HRESULT hr, const std::wstring &fn_name,
        const std::wstring &file_name, int lineno) : err_code(hr),
        fn_name(fn_name), file_name(file_name), lineno(lineno) {}

    std::wstring ToString() const {
        _com_error err(err_code);
        std::wstring msg = err.ErrorMessage();
        return fn_name + L" failed in " + file_name + L"; line " + std::to_wstring(lineno) + L"; error: " + msg;
    }

    HRESULT err_code = S_OK;
    std::wstring fn_name;
    std::wstring file_name;
    int lineno = -1;
};

inline std::wstring AnsiToWString(const std::string &str) {
    WCHAR buffer[512];
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, buffer, 512);
    return std::wstring(buffer);
}

#ifndef ThrowIfFailed
#define ThrowIfFailed(x) do {\
    HRESULT hr = (x); \
    std::wstring file = AnsiToWString(__FILE__); \
    if (FAILED(hr)) { \
        throw DxException(hr, L#x, file, __LINE__); \
    } \
} while(0)
#endif