// vtbl.h -- indices de vtable de las interfaces COM que gpuprobe
// engancha. GENERADO por tools/gen_vtbl.py desde los headers de la SDK:
// no editar a mano.
//
// Un indice equivocado no falla al compilar ni al enganchar: llama a OTRO
// metodo de la interfaz con los argumentos de este, y el juego se cae en un
// lugar que no tiene nada que ver. Por eso salen del header y no de una
// tabla escrita a mano.
//
// Se commitea: los indices de una vtable COM no pueden cambiar sin romper la
// ABI de Windows entera. Regenerar es para agregar interfaces, no para
// seguirle el paso a una SDK nueva.
#pragma once

namespace gp {
namespace vtbl {

// ID3D12Device -- 44 metodos
namespace Device {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int GetPrivateData = 3;
    inline constexpr int SetPrivateData = 4;
    inline constexpr int SetPrivateDataInterface = 5;
    inline constexpr int SetName = 6;
    inline constexpr int GetNodeCount = 7;
    inline constexpr int CreateCommandQueue = 8;
    inline constexpr int CreateCommandAllocator = 9;
    inline constexpr int CreateGraphicsPipelineState = 10;
    inline constexpr int CreateComputePipelineState = 11;
    inline constexpr int CreateCommandList = 12;
    inline constexpr int CheckFeatureSupport = 13;
    inline constexpr int CreateDescriptorHeap = 14;
    inline constexpr int GetDescriptorHandleIncrementSize = 15;
    inline constexpr int CreateRootSignature = 16;
    inline constexpr int CreateConstantBufferView = 17;
    inline constexpr int CreateShaderResourceView = 18;
    inline constexpr int CreateUnorderedAccessView = 19;
    inline constexpr int CreateRenderTargetView = 20;
    inline constexpr int CreateDepthStencilView = 21;
    inline constexpr int CreateSampler = 22;
    inline constexpr int CopyDescriptors = 23;
    inline constexpr int CopyDescriptorsSimple = 24;
    inline constexpr int GetResourceAllocationInfo = 25;
    inline constexpr int GetCustomHeapProperties = 26;
    inline constexpr int CreateCommittedResource = 27;
    inline constexpr int CreateHeap = 28;
    inline constexpr int CreatePlacedResource = 29;
    inline constexpr int CreateReservedResource = 30;
    inline constexpr int CreateSharedHandle = 31;
    inline constexpr int OpenSharedHandle = 32;
    inline constexpr int OpenSharedHandleByName = 33;
    inline constexpr int MakeResident = 34;
    inline constexpr int Evict = 35;
    inline constexpr int CreateFence = 36;
    inline constexpr int GetDeviceRemovedReason = 37;
    inline constexpr int GetCopyableFootprints = 38;
    inline constexpr int CreateQueryHeap = 39;
    inline constexpr int SetStablePowerState = 40;
    inline constexpr int CreateCommandSignature = 41;
    inline constexpr int GetResourceTiling = 42;
    inline constexpr int GetAdapterLuid = 43;
}

// ID3D12CommandQueue -- 19 metodos
namespace CommandQueue {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int GetPrivateData = 3;
    inline constexpr int SetPrivateData = 4;
    inline constexpr int SetPrivateDataInterface = 5;
    inline constexpr int SetName = 6;
    inline constexpr int GetDevice = 7;
    inline constexpr int UpdateTileMappings = 8;
    inline constexpr int CopyTileMappings = 9;
    inline constexpr int ExecuteCommandLists = 10;
    inline constexpr int SetMarker = 11;
    inline constexpr int BeginEvent = 12;
    inline constexpr int EndEvent = 13;
    inline constexpr int Signal = 14;
    inline constexpr int Wait = 15;
    inline constexpr int GetTimestampFrequency = 16;
    inline constexpr int GetClockCalibration = 17;
    inline constexpr int GetDesc = 18;
}

// ID3D12GraphicsCommandList -- 60 metodos
namespace GraphicsCommandList {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int GetPrivateData = 3;
    inline constexpr int SetPrivateData = 4;
    inline constexpr int SetPrivateDataInterface = 5;
    inline constexpr int SetName = 6;
    inline constexpr int GetDevice = 7;
    inline constexpr int GetType = 8;
    inline constexpr int Close = 9;
    inline constexpr int Reset = 10;
    inline constexpr int ClearState = 11;
    inline constexpr int DrawInstanced = 12;
    inline constexpr int DrawIndexedInstanced = 13;
    inline constexpr int Dispatch = 14;
    inline constexpr int CopyBufferRegion = 15;
    inline constexpr int CopyTextureRegion = 16;
    inline constexpr int CopyResource = 17;
    inline constexpr int CopyTiles = 18;
    inline constexpr int ResolveSubresource = 19;
    inline constexpr int IASetPrimitiveTopology = 20;
    inline constexpr int RSSetViewports = 21;
    inline constexpr int RSSetScissorRects = 22;
    inline constexpr int OMSetBlendFactor = 23;
    inline constexpr int OMSetStencilRef = 24;
    inline constexpr int SetPipelineState = 25;
    inline constexpr int ResourceBarrier = 26;
    inline constexpr int ExecuteBundle = 27;
    inline constexpr int SetDescriptorHeaps = 28;
    inline constexpr int SetComputeRootSignature = 29;
    inline constexpr int SetGraphicsRootSignature = 30;
    inline constexpr int SetComputeRootDescriptorTable = 31;
    inline constexpr int SetGraphicsRootDescriptorTable = 32;
    inline constexpr int SetComputeRoot32BitConstant = 33;
    inline constexpr int SetGraphicsRoot32BitConstant = 34;
    inline constexpr int SetComputeRoot32BitConstants = 35;
    inline constexpr int SetGraphicsRoot32BitConstants = 36;
    inline constexpr int SetComputeRootConstantBufferView = 37;
    inline constexpr int SetGraphicsRootConstantBufferView = 38;
    inline constexpr int SetComputeRootShaderResourceView = 39;
    inline constexpr int SetGraphicsRootShaderResourceView = 40;
    inline constexpr int SetComputeRootUnorderedAccessView = 41;
    inline constexpr int SetGraphicsRootUnorderedAccessView = 42;
    inline constexpr int IASetIndexBuffer = 43;
    inline constexpr int IASetVertexBuffers = 44;
    inline constexpr int SOSetTargets = 45;
    inline constexpr int OMSetRenderTargets = 46;
    inline constexpr int ClearDepthStencilView = 47;
    inline constexpr int ClearRenderTargetView = 48;
    inline constexpr int ClearUnorderedAccessViewUint = 49;
    inline constexpr int ClearUnorderedAccessViewFloat = 50;
    inline constexpr int DiscardResource = 51;
    inline constexpr int BeginQuery = 52;
    inline constexpr int EndQuery = 53;
    inline constexpr int ResolveQueryData = 54;
    inline constexpr int SetPredication = 55;
    inline constexpr int SetMarker = 56;
    inline constexpr int BeginEvent = 57;
    inline constexpr int EndEvent = 58;
    inline constexpr int ExecuteIndirect = 59;
}

// IDXGIFactory -- 12 metodos
namespace Factory {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int SetPrivateData = 3;
    inline constexpr int SetPrivateDataInterface = 4;
    inline constexpr int GetPrivateData = 5;
    inline constexpr int GetParent = 6;
    inline constexpr int EnumAdapters = 7;
    inline constexpr int MakeWindowAssociation = 8;
    inline constexpr int GetWindowAssociation = 9;
    inline constexpr int CreateSwapChain = 10;
    inline constexpr int CreateSoftwareAdapter = 11;
}

// IDXGIFactory2 -- 25 metodos
namespace Factory2 {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int SetPrivateData = 3;
    inline constexpr int SetPrivateDataInterface = 4;
    inline constexpr int GetPrivateData = 5;
    inline constexpr int GetParent = 6;
    inline constexpr int EnumAdapters = 7;
    inline constexpr int MakeWindowAssociation = 8;
    inline constexpr int GetWindowAssociation = 9;
    inline constexpr int CreateSwapChain = 10;
    inline constexpr int CreateSoftwareAdapter = 11;
    inline constexpr int EnumAdapters1 = 12;
    inline constexpr int IsCurrent = 13;
    inline constexpr int IsWindowedStereoEnabled = 14;
    inline constexpr int CreateSwapChainForHwnd = 15;
    inline constexpr int CreateSwapChainForCoreWindow = 16;
    inline constexpr int GetSharedResourceAdapterLuid = 17;
    inline constexpr int RegisterStereoStatusWindow = 18;
    inline constexpr int RegisterStereoStatusEvent = 19;
    inline constexpr int UnregisterStereoStatus = 20;
    inline constexpr int RegisterOcclusionStatusWindow = 21;
    inline constexpr int RegisterOcclusionStatusEvent = 22;
    inline constexpr int UnregisterOcclusionStatus = 23;
    inline constexpr int CreateSwapChainForComposition = 24;
}

// IDXGISwapChain -- 18 metodos
namespace SwapChain {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int SetPrivateData = 3;
    inline constexpr int SetPrivateDataInterface = 4;
    inline constexpr int GetPrivateData = 5;
    inline constexpr int GetParent = 6;
    inline constexpr int GetDevice = 7;
    inline constexpr int Present = 8;
    inline constexpr int GetBuffer = 9;
    inline constexpr int SetFullscreenState = 10;
    inline constexpr int GetFullscreenState = 11;
    inline constexpr int GetDesc = 12;
    inline constexpr int ResizeBuffers = 13;
    inline constexpr int ResizeTarget = 14;
    inline constexpr int GetContainingOutput = 15;
    inline constexpr int GetFrameStatistics = 16;
    inline constexpr int GetLastPresentCount = 17;
}

// IDXGISwapChain1 -- 29 metodos
namespace SwapChain1 {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int SetPrivateData = 3;
    inline constexpr int SetPrivateDataInterface = 4;
    inline constexpr int GetPrivateData = 5;
    inline constexpr int GetParent = 6;
    inline constexpr int GetDevice = 7;
    inline constexpr int Present = 8;
    inline constexpr int GetBuffer = 9;
    inline constexpr int SetFullscreenState = 10;
    inline constexpr int GetFullscreenState = 11;
    inline constexpr int GetDesc = 12;
    inline constexpr int ResizeBuffers = 13;
    inline constexpr int ResizeTarget = 14;
    inline constexpr int GetContainingOutput = 15;
    inline constexpr int GetFrameStatistics = 16;
    inline constexpr int GetLastPresentCount = 17;
    inline constexpr int GetDesc1 = 18;
    inline constexpr int GetFullscreenDesc = 19;
    inline constexpr int GetHwnd = 20;
    inline constexpr int GetCoreWindow = 21;
    inline constexpr int Present1 = 22;
    inline constexpr int IsTemporaryMonoSupported = 23;
    inline constexpr int GetRestrictToOutput = 24;
    inline constexpr int SetBackgroundColor = 25;
    inline constexpr int GetBackgroundColor = 26;
    inline constexpr int SetRotation = 27;
    inline constexpr int GetRotation = 28;
}

// IDXGISwapChain3 -- 40 metodos
namespace SwapChain3 {
    inline constexpr int QueryInterface = 0;
    inline constexpr int AddRef = 1;
    inline constexpr int Release = 2;
    inline constexpr int SetPrivateData = 3;
    inline constexpr int SetPrivateDataInterface = 4;
    inline constexpr int GetPrivateData = 5;
    inline constexpr int GetParent = 6;
    inline constexpr int GetDevice = 7;
    inline constexpr int Present = 8;
    inline constexpr int GetBuffer = 9;
    inline constexpr int SetFullscreenState = 10;
    inline constexpr int GetFullscreenState = 11;
    inline constexpr int GetDesc = 12;
    inline constexpr int ResizeBuffers = 13;
    inline constexpr int ResizeTarget = 14;
    inline constexpr int GetContainingOutput = 15;
    inline constexpr int GetFrameStatistics = 16;
    inline constexpr int GetLastPresentCount = 17;
    inline constexpr int GetDesc1 = 18;
    inline constexpr int GetFullscreenDesc = 19;
    inline constexpr int GetHwnd = 20;
    inline constexpr int GetCoreWindow = 21;
    inline constexpr int Present1 = 22;
    inline constexpr int IsTemporaryMonoSupported = 23;
    inline constexpr int GetRestrictToOutput = 24;
    inline constexpr int SetBackgroundColor = 25;
    inline constexpr int GetBackgroundColor = 26;
    inline constexpr int SetRotation = 27;
    inline constexpr int GetRotation = 28;
    inline constexpr int SetSourceSize = 29;
    inline constexpr int GetSourceSize = 30;
    inline constexpr int SetMaximumFrameLatency = 31;
    inline constexpr int GetMaximumFrameLatency = 32;
    inline constexpr int GetFrameLatencyWaitableObject = 33;
    inline constexpr int SetMatrixTransform = 34;
    inline constexpr int GetMatrixTransform = 35;
    inline constexpr int GetCurrentBackBufferIndex = 36;
    inline constexpr int CheckColorSpaceSupport = 37;
    inline constexpr int SetColorSpace1 = 38;
    inline constexpr int ResizeBuffers1 = 39;
}

}  // namespace vtbl
}  // namespace gp
