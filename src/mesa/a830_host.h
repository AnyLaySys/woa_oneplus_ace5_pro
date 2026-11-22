#pragma once
#include <windows.h>
#include <stdint.h>
/* Process-local callbacks; no pointers from this ABI are passed to the kernel. */
#define A830_HOST_VERSION 5
typedef struct {
   void *Token;
   uint64_t Address;
   unsigned Size,Pitch;
} A830_IMAGE;
typedef struct {
   unsigned Version,Size;
   void *User;
   HRESULT (WINAPI *Create)(void *,HANDLE,HANDLE *,void **);
   HRESULT (WINAPI *Render)(void *,const void *,UINT);
   void (WINAPI *Destroy)(void *);
   HRESULT (WINAPI *Synchronize)(void *,unsigned,uint64_t,BOOL);
   HRESULT (WINAPI *OpenImage)(void *,void *,A830_IMAGE *);
   HRESULT (WINAPI *QueryImage)(void *,A830_IMAGE *);
   void (WINAPI *CloseImage)(void *,A830_IMAGE *);
} A830_HOST;
typedef void (WINAPI *A830_SET_HOST)(const A830_HOST *);
