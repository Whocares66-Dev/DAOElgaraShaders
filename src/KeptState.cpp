#include "KeptState.h"

namespace keptstate
{

Kept::Kept(IDirect3DDevice9 *device, IDirect3DStateBlock9 *&block, GetTargetFn get, SetTargetFn set)
    : device_(device), set_(set)
{
    if (block == nullptr && FAILED(device->CreateStateBlock(D3DSBT_ALL, &block)))
    {
        block = nullptr;
        return;
    }
    block_ = block;
    block_->Capture();
    for (UINT at = 0; at < streams_.size(); ++at)
    {
        Stream &stream = streams_.at(at);
        stream.read = SUCCEEDED(device->GetStreamSource(at, &stream.buffer, &stream.offset, &stream.stride));
    }
    for (UINT at = 0; at < targets_.size(); ++at)
    {
        if (get != nullptr)
        {
            get(device, at, &targets_.at(at));
        }
        else
        {
            device->GetRenderTarget(at, &targets_.at(at));
        }
    }
    device->GetDepthStencilSurface(&depth_);
}

Kept::~Kept()
{
    if (block_ == nullptr)
    {
        return;
    }
    for (UINT at = 0; at < targets_.size(); ++at)
    {
        IDirect3DSurface9 *&target = targets_.at(at);
        if (set_ != nullptr)
        {
            set_(device_, at, target);
        }
        else
        {
            device_->SetRenderTarget(at, target);
        }
        if (target != nullptr)
        {
            target->Release();
            target = nullptr;
        }
    }
    device_->SetDepthStencilSurface(depth_);
    if (depth_ != nullptr)
    {
        depth_->Release();
        depth_ = nullptr;
    }
    block_->Apply();
    for (UINT at = 0; at < streams_.size(); ++at)
    {
        Stream &stream = streams_.at(at);
        if (stream.read)
        {
            device_->SetStreamSource(at, stream.buffer, stream.offset, stream.stride);
        }
        if (stream.buffer != nullptr)
        {
            stream.buffer->Release();
            stream.buffer = nullptr;
        }
    }
}

} // namespace keptstate
