#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <wchar.h>

template<class T> static void Release(T *&p) {if(p){p->Release();p=nullptr;}}

int wmain(int argc,wchar_t **argv) {
    const DWORD firstVideo=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    const DWORD allStreams=static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
    if(argc>2){fwprintf(stderr,L"Usage: qcom_cam_check [FRAME.nv12]\n");return 2;}
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))return 1;
    hr=MFStartup(MF_VERSION);if(FAILED(hr)){CoUninitialize();return 1;}
    IMFAttributes *attributes=nullptr,*readerAttributes=nullptr;IMFActivate **devices=nullptr;UINT32 count=0,index=UINT32_MAX;
    IMFMediaSource *source=nullptr;IMFSourceReader *reader=nullptr;IMFMediaType *type=nullptr;
    IMFSample *sample=nullptr;IMFMediaBuffer *buffer=nullptr;
    const wchar_t *step=L"enumerate";
    hr=MFCreateAttributes(&attributes,1);
    if(SUCCEEDED(hr))hr=attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if(SUCCEEDED(hr))hr=MFEnumDeviceSources(attributes,&devices,&count);
    if(SUCCEEDED(hr)) {
        wprintf(L"Windows video sources: %u\n",count);
        for(UINT32 i=0;i<count;i++) {
            wchar_t *name=nullptr;UINT32 length=0;
            if(SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,&name,&length))) {
                wprintf(L"%u: %ls\n",i,name);if(wcsstr(name,L"IMX906"))index=i;CoTaskMemFree(name);
            }
        }
        if(index==UINT32_MAX)hr=MF_E_NOT_FOUND;
    }
    if(SUCCEEDED(hr)&&argc==2) {
        step=L"activate source";
        hr=devices[index]->ActivateObject(IID_PPV_ARGS(&source));
        if(SUCCEEDED(hr))hr=MFCreateAttributes(&readerAttributes,2);
        if(SUCCEEDED(hr))hr=readerAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA,TRUE);
        if(SUCCEEDED(hr))hr=readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,TRUE);
        step=SUCCEEDED(hr)?L"create source reader":step;
        if(SUCCEEDED(hr))hr=MFCreateSourceReaderFromMediaSource(source,readerAttributes,&reader);
        step=SUCCEEDED(hr)?L"select stream":step;
        if(SUCCEEDED(hr))hr=reader->SetStreamSelection(allStreams,FALSE);
        if(SUCCEEDED(hr))hr=reader->SetStreamSelection(firstVideo,TRUE);
        if(SUCCEEDED(hr))hr=MFCreateMediaType(&type);
        if(SUCCEEDED(hr))hr=type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
        if(SUCCEEDED(hr))hr=type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12);
        if(SUCCEEDED(hr))hr=MFSetAttributeSize(type,MF_MT_FRAME_SIZE,1024,768);
        if(SUCCEEDED(hr))hr=MFSetAttributeRatio(type,MF_MT_FRAME_RATE,1,1);
        step=SUCCEEDED(hr)?L"set NV12 format":step;
        if(SUCCEEDED(hr))hr=reader->SetCurrentMediaType(firstVideo,nullptr,type);
        DWORD stream=0,flags=0;LONGLONG time=0;
        step=SUCCEEDED(hr)?L"read sample":step;
        if(SUCCEEDED(hr))hr=reader->ReadSample(firstVideo,0,&stream,&flags,&time,&sample);
        if(SUCCEEDED(hr)&&(!sample||(flags&MF_SOURCE_READERF_ENDOFSTREAM)))hr=MF_E_END_OF_STREAM;
        if(SUCCEEDED(hr))hr=sample->ConvertToContiguousBuffer(&buffer);
        if(SUCCEEDED(hr)) {
            BYTE *data=nullptr;DWORD maximum=0,bytes=0;hr=buffer->Lock(&data,&maximum,&bytes);
            if(SUCCEEDED(hr)) {
                if(bytes!=1024*768*3/2)hr=MF_E_INVALIDMEDIATYPE;
                else {
                    HANDLE file=CreateFileW(argv[1],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                    if(file==INVALID_HANDLE_VALUE)hr=HRESULT_FROM_WIN32(GetLastError());
                    else {
                        DWORD written=0;
                        if(!WriteFile(file,data,bytes,&written,nullptr)||written!=bytes)hr=HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
                        CloseHandle(file);
                        if(SUCCEEDED(hr))wprintf(L"Captured NV12: 1024x768, %lu bytes, timestamp=%lld\n",bytes,time);
                    }
                }
                buffer->Unlock();
            }
        }
    }
    if(FAILED(hr))fwprintf(stderr,L"Camera check failed at %ls: 0x%08lx\n",step,static_cast<ULONG>(hr));
    Release(buffer);Release(sample);Release(type);Release(reader);
    if(source)source->Shutdown();Release(source);
    if(devices){for(UINT32 i=0;i<count;i++)devices[i]->Release();CoTaskMemFree(devices);}
    Release(readerAttributes);Release(attributes);MFShutdown();CoUninitialize();return SUCCEEDED(hr)?0:1;
}
