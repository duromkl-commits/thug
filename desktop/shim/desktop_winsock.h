// Winsock for the engine's network code (Code/Gel/Net/net.h), without letting
// <windows.h> leak min/max and friends into the 2003 code.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>

// <windows.h> renames API functions with A/W macros, which then rewrite the
// engine's own methods of the same name (CObject::GetObject -> GetObjectA...).
#undef GetObject
#undef GetMessage
#undef SendMessage
#undef PostMessage
#undef CreateFile
#undef DeleteFile
#undef CopyFile
#undef MoveFile
#undef CreateDirectory
#undef RemoveDirectory
#undef FindFirstFile
#undef FindNextFile
#undef GetCurrentDirectory
#undef SetCurrentDirectory
#undef GetCurrentTime
#undef LoadImage
#undef DrawText
#undef PlaySound
#undef GetClassName
#undef CreateWindow
#undef CreateEvent
#undef CreateMutex
#undef CreateSemaphore
#undef GetUserName
#undef GetComputerName
#undef GetCommandLine
#undef GetObjectType
#undef Yield
#undef near
#undef far
#undef IN
#undef OUT
#undef ERROR
#undef DELETE
#undef interface
#undef RGB
#undef AddJob
#undef StartDoc
#undef GetProp
#undef SetProp
#undef RemoveProp
#undef GetFileAttributes
#undef UpdateResource
#undef FreeResource
#undef LoadResource
#undef FindResource
#undef ReportEvent
#undef OpenEvent
#undef GetFreeSpace
#undef GetTempPath
#undef GetModuleFileName
#undef CreateProcess
#undef GetFileTitle
#undef ChooseColor
#undef small
#undef PASCAL
#undef CALLBACK
