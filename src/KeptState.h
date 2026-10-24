// The device's state kept across a draw of the plugin's own made mid-frame
// (the sun's shadow maps, the effects' lights' maps, the picture copied onto
// the back buffer) and put back whole as the draw's scope ends: a state
// block's, the vertex streams where they were bound, and the targets.
//
// It was put back by hand in three places, and once incompletely: a state
// block's Apply puts a stream's buffer back at offset 0, not where it was
// bound. The engine binds a buffer it holds many meshes in at each mesh's
// offset (stream 0 at 1392 bytes, read 2026-10-24) and, filtering what it set
// already, does not bind it again, so the draws after the effects' maps read
// their vertices from the buffer's start: whole meshes drawn black as the
// camera turned (the Caverns' statues and walls, the Ruined Temple's snow;
// the user, 2026-10-24: "the wall in the room turns black at certain camera
// angles", and "the save & restore should be factored out nonetheless since
// it's used in at least three places").
#pragma once

#include <d3d9.h>

#include <array>

namespace keptstate
{

// The device's own GetRenderTarget and SetRenderTarget, where the caller is
// to pass Graphics.cpp's detours of them by (the picture's copy, which sets
// the swap chain's own back buffer); none for the device's methods.
using GetTargetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 **);
using SetTargetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);

// While it lives, the plugin may change the device's state as it will; as it
// goes, the targets are bound again (first: setting a target resets the
// viewport), then the state block applied, then the streams bound again.
class Kept
{
  public:
    // @param block the caller's state block, made here where it is none yet
    // (D3DSBT_ALL) and let go by the caller, as a Reset of the device needs
    Kept(IDirect3DDevice9 *device, IDirect3DStateBlock9 *&block, GetTargetFn get = nullptr, SetTargetFn set = nullptr);
    ~Kept();
    Kept(const Kept &) = delete;
    Kept &operator=(const Kept &) = delete;
    Kept(Kept &&) = delete;
    Kept &operator=(Kept &&) = delete;

    // Whether the state was kept: false where no state block could be made,
    // and nothing is to be drawn then.
    explicit operator bool() const
    {
        return block_ != nullptr;
    }

  private:
    struct Stream
    {
        IDirect3DVertexBuffer9 *buffer = nullptr;
        UINT offset = 0;
        UINT stride = 0;
        bool read = false;
    };

    IDirect3DDevice9 *device_;
    IDirect3DStateBlock9 *block_ = nullptr;
    SetTargetFn set_;
    std::array<Stream, 16> streams_{};
    std::array<IDirect3DSurface9 *, 2> targets_{};
    IDirect3DSurface9 *depth_ = nullptr;
};

} // namespace keptstate
