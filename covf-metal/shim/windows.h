#pragma once
// Minimal Windows shim for the portable CPU core of CoAVolFog on macOS.
#include <cstdint>
#include <cstddef>
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* HANDLE;
typedef unsigned char BYTE;
typedef unsigned short WORD;
#define __cdecl
#define __stdcall
#define WINAPI
// Virtual-key codes used by config.cpp
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HOME 0x24
#define VK_END 0x23
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_PAUSE 0x13
#define VK_SCROLL 0x91
#define VK_F1 0x70
#define VK_F24 0x87
#define VK_CONTROL 0x11
#define VK_SHIFT 0x10
#define VK_MENU 0x12
#define VK_MEDIA_PREV_TRACK 0xB1
#define VK_MEDIA_NEXT_TRACK 0xB0
#define VK_MEDIA_PLAY_PAUSE 0xB3
#define VK_MEDIA_STOP 0xB2
#define VK_VOLUME_MUTE 0xAD
#define VK_VOLUME_DOWN 0xAE
#define VK_VOLUME_UP 0xAF
