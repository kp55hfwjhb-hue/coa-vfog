#pragma once
// Minimal D3D9 type shim — only what the portable core references.
#include "windows.h"
struct D3DVIEWPORT9 { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
struct IDirect3DDevice9; struct IDirect3DSurface9; struct IDirect3D9;
struct IDirect3DVolume9;
struct D3DMATRIX { float m[4][4]; };
