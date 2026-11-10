// Issue 80, 2026-11-10: reproduce Steam rehooking a Present vtable replacement.
// A standalone x86 D3D9 program: no game, loader or plugin required. See issue-80.md.
#include <Windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

using PresentFn = HRESULT(WINAPI *)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
PresentFn original = nullptr;
unsigned depth = 0;
unsigned calls = 0;
bool recursed = false;

HRESULT WINAPI Present(IDirect3DSwapChain9 *chain, const RECT *from, const RECT *to, HWND window, const RGNDATA *dirty,
                       DWORD flags)
{
    ++calls;
    if (++depth > 8)
    {
        --depth;
        recursed = true;
        std::puts("RECURSION: stopped at depth 9 instead of overflowing the stack");
        return E_FAIL;
    }
    const HRESULT result = original(chain, from, to, window, dirty, flags);
    --depth;
    return result;
}

void Describe(const char *name, void *address)
{
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<const char *>(address), &module);
    if (module != nullptr)
        GetModuleFileNameA(module, path, MAX_PATH);
    const auto *bytes = static_cast<const unsigned char *>(address);
    std::printf("%s: %p %s: %02x %02x %02x %02x %02x\n", name, address, module != nullptr ? path : "private memory",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool steam = false;
    bool hook = false;
    bool requery = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "steam") == 0)
            steam = true;
        else if (std::strcmp(argv[i], "hook") == 0)
            hook = true;
        else if (std::strcmp(argv[i], "requery") == 0)
            requery = true;
        else
            return 1;
    }
    if (steam)
    {
        // These affect only this test process. Steam itself must already be running.
        SetEnvironmentVariableA("SteamAppId", "47810");
        SetEnvironmentVariableA("SteamGameId", "47810");
        if (LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\GameOverlayRenderer.dll") == nullptr)
        {
            std::printf("Steam overlay could not load: %lu\n", GetLastError());
            return 1;
        }
    }
    const HWND window = CreateWindowExW(0, L"STATIC", L"Issue 80 D3D9 probe", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d == nullptr || window == nullptr)
        return 1;
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferWidth = 320;
    parameters.BackBufferHeight = 240;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *device = nullptr;
    HRESULT result =
        d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    std::printf("CreateDevice: %08lx\n", result);
    if (FAILED(result) || device == nullptr)
        return 1;
    IDirect3DSwapChain9 *chain = nullptr;
    if (FAILED(device->GetSwapChain(0, &chain)) || chain == nullptr)
        return 1;
    void **entry = *reinterpret_cast<void ***>(chain) + 3;
    original = reinterpret_cast<PresentFn>(*entry);
    Describe("original before", reinterpret_cast<void *>(original));
    Describe("callback before", reinterpret_cast<void *>(&Present));
    if (hook)
    {
        // Same shared-slot replacement as dao::VtableHook::Attach, without library dependencies.
        DWORD protection = 0;
        if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &protection))
            return 1;
        InterlockedExchangePointer(entry, reinterpret_cast<void *>(&Present));
        VirtualProtect(entry, sizeof(void *), protection, &protection);
    }
    if (requery)
    {
        IDirect3DSwapChain9 *again = nullptr;
        result = device->GetSwapChain(0, &again);
        std::printf("GetSwapChain again: %08lx\n", result);
        if (FAILED(result) || again == nullptr)
            return 1;
        again->Release();
    }
    Describe("original after query", reinterpret_cast<void *>(original));
    Describe("callback after query", reinterpret_cast<void *>(&Present));
    for (int i = 0; i < 120; ++i)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff204060, 1, 0);
        result = chain->Present(nullptr, nullptr, window, nullptr, 0);
        if (FAILED(result))
        {
            std::printf("frame %d: %08lx, %u callback calls\n", i, result, calls);
            return recursed ? 2 : 1;
        }
        Sleep(20);
    }
    std::printf("120 frames completed, %u callback calls\n", calls);
    // Deliberately leave the objects and hook until process exit: an overlay can still hold
    // a trampoline to the replacement. There is no production unload policy in this probe.
    return 0;
}
