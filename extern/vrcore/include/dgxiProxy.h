// vrcore - dxgi.dll proxy. The mod is loaded by the game as dxgi.dll; these exports forward
// to the real system dxgi.dll. Include this header from exactly one .cpp file (Main.cpp).
#pragma once

#include <Windows.h>

#include <dxgi1_2.h>

#include <mutex>
#include <string>

namespace vrcore::dxgi_proxy {
inline HMODULE real_dxgi() {
    static HMODULE module = []() -> HMODULE {
        wchar_t system_dir[MAX_PATH]{};
        GetSystemDirectoryW(system_dir, MAX_PATH);

        const auto path = std::wstring{system_dir} + L"\\dxgi.dll";
        return LoadLibraryW(path.c_str());
    }();

    return module;
}

template <typename T>
T real_proc(const char* name) {
    const auto module = real_dxgi();
    return module != nullptr ? (T)GetProcAddress(module, name) : nullptr;
}
} // namespace vrcore::dxgi_proxy

#pragma comment(linker, "/EXPORT:CreateDXGIFactory=vrcore_CreateDXGIFactory")
#pragma comment(linker, "/EXPORT:CreateDXGIFactory1=vrcore_CreateDXGIFactory1")
#pragma comment(linker, "/EXPORT:CreateDXGIFactory2=vrcore_CreateDXGIFactory2")
#pragma comment(linker, "/EXPORT:DXGIGetDebugInterface1=vrcore_DXGIGetDebugInterface1")
#pragma comment(linker, "/EXPORT:DXGIDeclareAdapterRemovalSupport=vrcore_DXGIDeclareAdapterRemovalSupport")
#pragma comment(linker, "/EXPORT:DXGIDisableVBlankVirtualization=vrcore_DXGIDisableVBlankVirtualization")

extern "C" {
HRESULT WINAPI vrcore_CreateDXGIFactory(REFIID riid, void** factory) {
    using fn_t = HRESULT(WINAPI*)(REFIID, void**);
    static auto fn = vrcore::dxgi_proxy::real_proc<fn_t>("CreateDXGIFactory");
    return fn != nullptr ? fn(riid, factory) : E_NOTIMPL;
}

HRESULT WINAPI vrcore_CreateDXGIFactory1(REFIID riid, void** factory) {
    using fn_t = HRESULT(WINAPI*)(REFIID, void**);
    static auto fn = vrcore::dxgi_proxy::real_proc<fn_t>("CreateDXGIFactory1");
    return fn != nullptr ? fn(riid, factory) : E_NOTIMPL;
}

HRESULT WINAPI vrcore_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    using fn_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
    static auto fn = vrcore::dxgi_proxy::real_proc<fn_t>("CreateDXGIFactory2");
    return fn != nullptr ? fn(flags, riid, factory) : E_NOTIMPL;
}

HRESULT WINAPI vrcore_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug) {
    using fn_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
    static auto fn = vrcore::dxgi_proxy::real_proc<fn_t>("DXGIGetDebugInterface1");
    return fn != nullptr ? fn(flags, riid, debug) : E_NOTIMPL;
}

HRESULT WINAPI vrcore_DXGIDeclareAdapterRemovalSupport() {
    using fn_t = HRESULT(WINAPI*)();
    static auto fn = vrcore::dxgi_proxy::real_proc<fn_t>("DXGIDeclareAdapterRemovalSupport");
    return fn != nullptr ? fn() : E_NOTIMPL;
}

HRESULT WINAPI vrcore_DXGIDisableVBlankVirtualization() {
    using fn_t = HRESULT(WINAPI*)();
    static auto fn = vrcore::dxgi_proxy::real_proc<fn_t>("DXGIDisableVBlankVirtualization");
    return fn != nullptr ? fn() : E_NOTIMPL;
}
}
