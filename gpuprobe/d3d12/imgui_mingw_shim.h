// imgui_mingw_shim.h -- lo que le falta al d3d12.h de mingw-w64 para compilar
// el backend de ImGui.
//
// Se pasa con -include SOLO al compilar las fuentes de ImGui, y solo con
// mingw. Con MSVC y la SDK de Windows no hace falta: el typedef existe ahi.
// No es un parche a ImGui -- es un hueco del header de mingw.
#pragma once

#include <d3d12.h>

#ifndef PFN_D3D12_SERIALIZE_ROOT_SIGNATURE
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(
    const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob **ppBlob,
    ID3DBlob **ppErrorBlob);
#endif
