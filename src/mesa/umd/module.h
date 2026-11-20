#pragma once
#if defined(_M_X64)
#define A830_MODULE(name) name L"_x64.dll"
#else
#define A830_MODULE(name) name L".dll"
#endif
