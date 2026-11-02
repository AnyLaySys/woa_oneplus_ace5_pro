#include <initguid.h>
#include <portcls.h>
#include <ksmedia.h>
#include "sia9177.h"

void *operator new(size_t Size) { return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, 'avWS'); }
void operator delete(void *Pointer) { if (Pointer) ExFreePoolWithTag(Pointer, 'avWS'); }
void operator delete(void *Pointer, size_t) { operator delete(Pointer); }

static NTSTATUS QueryInterface(PUNKNOWN Object, REFIID Requested, REFIID Type, REFIID Base, PVOID *Result)
{
    *Result = nullptr;
    if (!IsEqualGUIDAligned(Requested, IID_IUnknown) && !IsEqualGUIDAligned(Requested, Type) &&
        !IsEqualGUIDAligned(Requested, Base)) return STATUS_NOINTERFACE;
    *Result = Object;
    Object->AddRef();
    return STATUS_SUCCESS;
}

static KSDATARANGE_AUDIO PcmRange = {
    { sizeof(KSDATARANGE_AUDIO), 0, SIA_PCM_FRAME, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    2, 24, 24, SIA_PCM_RATE, SIA_PCM_RATE
};
static KSDATARANGE AnalogRange = {
    sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE PcmRanges[] = { (PKSDATARANGE)&PcmRange };
static PKSDATARANGE AnalogRanges[] = { &AnalogRange };
static KSDATAFORMAT_WAVEFORMATEXTENSIBLE PcmFormat = {
    { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, SIA_PCM_FRAME, 0,
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    { { WAVE_FORMAT_EXTENSIBLE, 2, SIA_PCM_RATE, SIA_PCM_RATE * SIA_PCM_FRAME, SIA_PCM_FRAME, 24, 22 },
      {24}, KSAUDIO_SPEAKER_STEREO, STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM) }
};

static NTSTATUS CheckFormat(PKSDATAFORMAT Format)
{
    if (!Format || Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(Format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(Format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return STATUS_NO_MATCH;
    auto Wave = &((PKSDATAFORMAT_WAVEFORMATEX)Format)->WaveFormatEx;
    if (Wave->nChannels != 2 || Wave->nSamplesPerSec != SIA_PCM_RATE || Wave->wBitsPerSample != 24 ||
        Wave->nBlockAlign != SIA_PCM_FRAME || Wave->nAvgBytesPerSec != SIA_PCM_RATE * SIA_PCM_FRAME) return STATUS_NO_MATCH;
    if (Wave->wFormatTag == WAVE_FORMAT_PCM) return STATUS_SUCCESS;
    if (Wave->wFormatTag != WAVE_FORMAT_EXTENSIBLE || Wave->cbSize < 22 ||
        Format->FormatSize < sizeof(PcmFormat)) return STATUS_NO_MATCH;
    auto Extended = (PWAVEFORMATEXTENSIBLE)Wave;
    return Extended->Samples.wValidBitsPerSample == 24 && Extended->dwChannelMask == KSAUDIO_SPEAKER_STEREO &&
        IsEqualGUIDAligned(Extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ? STATUS_SUCCESS : STATUS_NO_MATCH;
}

static NTSTATUS JackProperty(PPCPROPERTY_REQUEST Request)
{
    if (Request->InstanceSize < sizeof(ULONG) || *(ULONG *)Request->Instance != 1) return STATUS_INVALID_PARAMETER;
    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (Request->ValueSize < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)Request->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
        Request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    ULONG Size = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);
    if (!Request->ValueSize) { Request->ValueSize = Size; return STATUS_BUFFER_OVERFLOW; }
    if (Request->ValueSize < Size) return STATUS_BUFFER_TOO_SMALL;
    auto Multiple = (PKSMULTIPLE_ITEM)Request->Value;
    Multiple->Size = Size;
    Multiple->Count = 1;
    auto Jack = (PKSJACK_DESCRIPTION)(Multiple + 1);
    RtlZeroMemory(Jack, sizeof(*Jack));
    Jack->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
    Jack->ConnectionType = eConnTypeUnknown;
    Jack->GeoLocation = eGeoLocNotApplicable;
    Jack->GenLocation = eGenLocInternal;
    Jack->PortConnection = ePortConnIntegratedDevice;
    Jack->IsConnected = TRUE;
    Request->ValueSize = Size;
    return STATUS_SUCCESS;
}

static PCPROPERTY_ITEM JackItems[] = {
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION,
      KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, JackProperty }
};
DEFINE_PCAUTOMATION_TABLE_PROP(JackAutomation, JackItems);
static PCPIN_DESCRIPTOR WavePins[] = {
    {1, 1, 0, nullptr, {0, nullptr, 0, nullptr, 1, PcmRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, AnalogRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static PCNODE_DESCRIPTOR WaveNodes[] = {{0, nullptr, &KSNODETYPE_DAC, nullptr}};
static PCCONNECTION_DESCRIPTOR WaveConnections[] = {
    {PCFILTER_NODE, 0, 0, 1}, {0, 0, PCFILTER_NODE, 1}
};
static PCFILTER_DESCRIPTOR WaveFilter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), 2, WavePins, sizeof(PCNODE_DESCRIPTOR), 1, WaveNodes,
    2, WaveConnections, 0, nullptr
};
static PCPIN_DESCRIPTOR TopologyPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, AnalogRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, AnalogRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_SPEAKER, nullptr, 0}}
};
static PCCONNECTION_DESCRIPTOR TopologyConnections[] = {{PCFILTER_NODE, 0, PCFILTER_NODE, 1}};
static PCFILTER_DESCRIPTOR TopologyFilter = {
    0, &JackAutomation, sizeof(PCPIN_DESCRIPTOR), 2, TopologyPins, 0, 0, nullptr,
    1, TopologyConnections, 0, nullptr
};

class Stream final : public IMiniportWaveRTStreamNotification {
    LONG References = 1;
    PSIA_DEVICE Dev;
    PPORTWAVERTSTREAM Port;
    PMDL Mdl = nullptr;
    KSSTATE State = KSSTATE_STOP;
public:
    Stream(PSIA_DEVICE Device, PPORTWAVERTSTREAM StreamPort) : Dev(Device), Port(StreamPort) { Port->AddRef(); }
    ~Stream() {
        if (Mdl) FreeAudioBuffer(Mdl, Dev->PcmSize);
        KIRQL Irql;
        KeAcquireSpinLock(&Dev->NotifyLock, &Irql);
        Dev->NotifyEvent = nullptr;
        KeReleaseSpinLock(&Dev->NotifyLock, Irql);
        Port->Release();
        InterlockedExchange(&Dev->StreamAllocated, 0);
    }
    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Object) override {
        return ::QueryInterface(this, Id, IID_IMiniportWaveRTStreamNotification, IID_IMiniportWaveRTStream, Object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&References); }
    STDMETHODIMP_(ULONG) Release() override { auto Count = InterlockedDecrement(&References); if (!Count) delete this; return Count; }
    STDMETHODIMP SetFormat(PKSDATAFORMAT Format) override { return State == KSSTATE_STOP ? CheckFormat(Format) : STATUS_INVALID_DEVICE_STATE; }
    STDMETHODIMP SetState(KSSTATE NewState) override {
        if (NewState > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
        NTSTATUS Status = STATUS_SUCCESS;
        if (NewState == KSSTATE_RUN) Status = SiaStreamStart(Dev);
        else if (State == KSSTATE_RUN) SiaStreamStop(Dev);
        if (NewState == KSSTATE_STOP) { Dev->ReadOffset = 0; InterlockedExchange64(&Dev->PlayedBytes, 0); }
        if (NT_SUCCESS(Status)) State = NewState;
        SiaLog(Dev, "WaveRT state %u: 0x%08x", NewState, Status);
        return Status;
    }
    STDMETHODIMP GetPosition(PKSAUDIO_POSITION Position) override {
        if (!Dev->PcmSize) return STATUS_INVALID_DEVICE_STATE;
        Position->PlayOffset = (ULONGLONG)InterlockedCompareExchange64(&Dev->PlayedBytes, 0, 0) % Dev->PcmSize;
        Position->WriteOffset = Dev->ReadOffset;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP AllocateAudioBuffer(ULONG Requested, PMDL *Buffer, ULONG *Actual, ULONG *Offset, MEMORY_CACHING_TYPE *Cache) override {
        return AllocateBufferWithNotification(0, Requested, Buffer, Actual, Offset, Cache);
    }
    STDMETHODIMP AllocateBufferWithNotification(ULONG Count, ULONG Requested, PMDL *Buffer,
        ULONG *Actual, ULONG *Offset, MEMORY_CACHING_TYPE *Cache) override {
        if (Mdl || !Requested || Count > 2 || Requested > 1024 * 1024) return STATUS_INVALID_PARAMETER;
        ULONG Unit = SIA_PCM_BYTES * 2;
        ULONG Size = ((Requested + Unit - 1) / Unit) * Unit;
        if (Size <= 2 * Unit) Size = 3 * Unit;
        SiaLog(Dev, "WaveRT buffer: notifications %lu requested %lu actual %lu fifo %lu", Count, Requested, Size, Unit);
        PHYSICAL_ADDRESS Highest;
        Highest.QuadPart = MAXLONGLONG;
        Mdl = Port->AllocatePagesForMdl(Highest, Size);
        if (!Mdl) return STATUS_INSUFFICIENT_RESOURCES;
        Dev->Pcm = (PUCHAR)Port->MapAllocatedPages(Mdl, MmCached);
        if (!Dev->Pcm) { Port->FreePagesFromMdl(Mdl); Mdl = nullptr; return STATUS_INSUFFICIENT_RESOURCES; }
        RtlZeroMemory(Dev->Pcm, Size);
        Dev->PcmSize = Size;
        Dev->NotifyBytes = Count ? Size / Count : 0;
        *Buffer = Mdl;
        *Actual = Size;
        *Offset = 0;
        *Cache = MmCached;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(VOID) FreeAudioBuffer(PMDL Buffer, ULONG Size) override {
        UNREFERENCED_PARAMETER(Size);
        if (!Buffer || Buffer != Mdl) return;
        SiaStreamStop(Dev);
        Port->UnmapAllocatedPages(Dev->Pcm, Mdl);
        Port->FreePagesFromMdl(Mdl);
        Dev->Pcm = nullptr;
        Dev->PcmSize = 0;
        Mdl = nullptr;
    }
    STDMETHODIMP_(VOID) FreeBufferWithNotification(PMDL Buffer, ULONG Size) override { FreeAudioBuffer(Buffer, Size); }
    STDMETHODIMP_(VOID) GetHWLatency(PKSRTAUDIO_HWLATENCY Latency) override {
        RtlZeroMemory(Latency, sizeof(*Latency));
        Latency->FifoSize = 2 * SIA_PCM_BYTES;
        SiaLog(Dev, "WaveRT latency: fifo %lu chipset %lu codec %lu", Latency->FifoSize, Latency->ChipsetDelay, Latency->CodecDelay);
    }
    STDMETHODIMP GetPositionRegister(PKSRTAUDIO_HWREGISTER Register) override { UNREFERENCED_PARAMETER(Register); return STATUS_NOT_SUPPORTED; }
    STDMETHODIMP GetClockRegister(PKSRTAUDIO_HWREGISTER Register) override { UNREFERENCED_PARAMETER(Register); return STATUS_NOT_SUPPORTED; }
    STDMETHODIMP RegisterNotificationEvent(PKEVENT Event) override {
        KIRQL Irql;
        KeAcquireSpinLock(&Dev->NotifyLock, &Irql);
        NTSTATUS Status = Dev->NotifyEvent && Dev->NotifyEvent != Event ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
        if (NT_SUCCESS(Status)) Dev->NotifyEvent = Event;
        KeReleaseSpinLock(&Dev->NotifyLock, Irql);
        return Status;
    }
    STDMETHODIMP UnregisterNotificationEvent(PKEVENT Event) override {
        KIRQL Irql;
        KeAcquireSpinLock(&Dev->NotifyLock, &Irql);
        if (Dev->NotifyEvent == Event) Dev->NotifyEvent = nullptr;
        KeReleaseSpinLock(&Dev->NotifyLock, Irql);
        return STATUS_SUCCESS;
    }
};

class Wave final : public IMiniportWaveRT {
    LONG References = 1;
    PSIA_DEVICE Dev;
public:
    explicit Wave(PSIA_DEVICE Device) : Dev(Device) {}
    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Object) override {
        return ::QueryInterface(this, Id, IID_IMiniportWaveRT, IID_IMiniport, Object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&References); }
    STDMETHODIMP_(ULONG) Release() override { auto Count = InterlockedDecrement(&References); if (!Count) delete this; return Count; }
    STDMETHODIMP Init(PUNKNOWN Adapter, PRESOURCELIST Resources, PPORTWAVERT Port) override {
        UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Resources); UNREFERENCED_PARAMETER(Port); return STATUS_SUCCESS;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *Description) override { *Description = &WaveFilter; return STATUS_SUCCESS; }
    STDMETHODIMP DataRangeIntersection(ULONG Pin, PKSDATARANGE Range, PKSDATARANGE Match,
        ULONG Size, PVOID Result, PULONG Length) override {
        UNREFERENCED_PARAMETER(Match);
        if (Pin != 0 || Range->FormatSize < sizeof(KSDATARANGE_AUDIO)) return STATUS_NO_MATCH;
        auto Audio = (PKSDATARANGE_AUDIO)Range;
        if (Audio->MaximumChannels < 2 || Audio->MinimumBitsPerSample > 24 || Audio->MaximumBitsPerSample < 24 ||
            Audio->MinimumSampleFrequency > SIA_PCM_RATE || Audio->MaximumSampleFrequency < SIA_PCM_RATE) return STATUS_NO_MATCH;
        *Length = sizeof(PcmFormat);
        if (!Size) return STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof(PcmFormat)) return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(Result, &PcmFormat, sizeof(PcmFormat)); return STATUS_SUCCESS;
    }
    STDMETHODIMP NewStream(PMINIPORTWAVERTSTREAM *Result, PPORTWAVERTSTREAM Port,
        ULONG Pin, BOOLEAN Capture, PKSDATAFORMAT Format) override {
        *Result = nullptr;
        SiaLog(Dev, "WaveRT open: pin %lu capture %u", Pin, Capture);
        if (Pin || Capture || !NT_SUCCESS(CheckFormat(Format))) return STATUS_INVALID_PARAMETER;
        if (InterlockedCompareExchange(&Dev->StreamAllocated, 1, 0)) return STATUS_DEVICE_BUSY;
        auto Created = new Stream(Dev, Port);
        if (!Created) { InterlockedExchange(&Dev->StreamAllocated, 0); return STATUS_INSUFFICIENT_RESOURCES; }
        *Result = Created;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP GetDeviceDescription(PDEVICE_DESCRIPTION Description) override {
        RtlZeroMemory(Description, sizeof(*Description));
        Description->Version = DEVICE_DESCRIPTION_VERSION;
        Description->Master = TRUE;
        Description->ScatterGather = TRUE;
        Description->Dma64BitAddresses = TRUE;
        Description->InterfaceType = Internal;
        Description->MaximumLength = 1024 * 1024;
        return STATUS_SUCCESS;
    }
};

class Topology final : public IMiniportTopology {
    LONG References = 1;
public:
    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Object) override {
        return ::QueryInterface(this, Id, IID_IMiniportTopology, IID_IMiniport, Object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&References); }
    STDMETHODIMP_(ULONG) Release() override { auto Count = InterlockedDecrement(&References); if (!Count) delete this; return Count; }
    STDMETHODIMP Init(PUNKNOWN Adapter, PRESOURCELIST Resources, PPORTTOPOLOGY Port) override {
        UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Resources); UNREFERENCED_PARAMETER(Port); return STATUS_SUCCESS;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *Description) override { *Description = &TopologyFilter; return STATUS_SUCCESS; }
    STDMETHODIMP DataRangeIntersection(ULONG Pin, PKSDATARANGE Range, PKSDATARANGE Match,
        ULONG Size, PVOID Result, PULONG Length) override {
        UNREFERENCED_PARAMETER(Pin); UNREFERENCED_PARAMETER(Range); UNREFERENCED_PARAMETER(Match);
        UNREFERENCED_PARAMETER(Size); UNREFERENCED_PARAMETER(Result); UNREFERENCED_PARAMETER(Length); return STATUS_NOT_IMPLEMENTED;
    }
};

struct Extension { UCHAR PortData[PORT_CLASS_DEVICE_EXTENSION_SIZE]; SIA_DEVICE Dev; };
static PSIA_DEVICE DeviceContext(PDEVICE_OBJECT Device) { return &((Extension *)Device->DeviceExtension)->Dev; }

static NTSTATUS Install(PDEVICE_OBJECT Device, PIRP Irp, PRESOURCELIST Resources,
    REFGUID Class, PWSTR Name, PUNKNOWN Miniport, PPORT *Port)
{
    NTSTATUS Status = PcNewPort(Port, Class);
    if (NT_SUCCESS(Status)) Status = (*Port)->Init(Device, Irp, Miniport, nullptr, Resources);
    if (NT_SUCCESS(Status)) Status = PcRegisterSubdevice(Device, Name, *Port);
    Miniport->Release();
    return Status;
}

static NTSTATUS StartDevice(PDEVICE_OBJECT Device, PIRP Irp, PRESOURCELIST Resources)
{
    auto Memory = Resources->NumberOfEntriesOfType(CmResourceTypeMemory) ?
        Resources->FindTranslatedEntry(CmResourceTypeMemory, 0) : nullptr;
    if (!Memory) return STATUS_DEVICE_CONFIGURATION_ERROR;
    auto Dev = DeviceContext(Device);
    NTSTATUS Status = SiaHardwareStart(Dev, Memory);
    if (!NT_SUCCESS(Status)) { SiaHardwareStop(Dev); return Status; }
    auto Top = new Topology;
    auto Render = new Wave(Dev);
    if (!Top || !Render) { if (Top) Top->Release(); if (Render) Render->Release(); SiaHardwareStop(Dev); return STATUS_INSUFFICIENT_RESOURCES; }
    PPORT TopPort = nullptr, WavePort = nullptr;
    Status = Install(Device, Irp, Resources, CLSID_PortTopology, L"Topology", Top, &TopPort);
    if (NT_SUCCESS(Status)) Status = Install(Device, Irp, Resources, CLSID_PortWaveRT, L"Wave", Render, &WavePort);
    else Render->Release();
    if (NT_SUCCESS(Status)) Status = PcRegisterPhysicalConnection(Device, WavePort, 1, TopPort, 0);
    if (TopPort) TopPort->Release();
    if (WavePort) WavePort->Release();
    if (!NT_SUCCESS(Status)) SiaHardwareStop(Dev);
    return Status;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Device)
{
    return PcAddAdapterDevice(Driver, Device, StartDevice, 2, sizeof(Extension));
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp)
{
    auto Stack = IoGetCurrentIrpStackLocation(Irp);
    if (Stack->MinorFunction == IRP_MN_STOP_DEVICE || Stack->MinorFunction == IRP_MN_REMOVE_DEVICE ||
        Stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL) SiaHardwareStop(DeviceContext(Device));
    return PcDispatchIrp(Device, Irp);
}

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status = PcInitializeAdapterDriver(Driver, RegistryPath, AddDevice);
    if (NT_SUCCESS(Status)) Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    return Status;
}
