#pragma once
#include <windows.h>
/* Process-local callbacks; no pointers from this ABI are passed to the kernel. */
typedef struct {
   unsigned Version,Size;
   void *User;
   HRESULT (WINAPI *Create)(void *,HANDLE,HANDLE *,void **);
   HRESULT (WINAPI *Render)(void *,const void *,UINT);
   void (WINAPI *Destroy)(void *);
} A830_HOST;
typedef void (WINAPI *A830_SET_HOST)(const A830_HOST *);
