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

static NTSTATUS CheckFormat(PKSDATAFORMAT Format, BOOLEAN Capture = FALSE)
{
    if (!Format || Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(Format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(Format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return STATUS_NO_MATCH;
    auto Wave = &((PKSDATAFORMAT_WAVEFORMATEX)Format)->WaveFormatEx;
    if (Wave->nChannels != 2 || Wave->nSamplesPerSec != SIA_PCM_RATE || Wave->wBitsPerSample != (Capture ? 16 : 24) ||
        Wave->nBlockAlign != (Capture ? SIA_CAPTURE_FRAME : SIA_PCM_FRAME) || Wave->nAvgBytesPerSec != SIA_PCM_RATE * (Capture ? SIA_CAPTURE_FRAME : SIA_PCM_FRAME)) return STATUS_NO_MATCH;
    if (Wave->wFormatTag == WAVE_FORMAT_PCM) return STATUS_SUCCESS;
    if (Wave->wFormatTag != WAVE_FORMAT_EXTENSIBLE || Wave->cbSize < 22 ||
        Format->FormatSize < sizeof(PcmFormat)) return STATUS_NO_MATCH;
    auto Extended = (PWAVEFORMATEXTENSIBLE)Wave;
    return Extended->Samples.wValidBitsPerSample == (Capture ? 16 : 24) && Extended->dwChannelMask == KSAUDIO_SPEAKER_STEREO &&
        IsEqualGUIDAligned(Extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ? STATUS_SUCCESS : STATUS_NO_MATCH;
}

static KSDATARANGE_AUDIO CaptureRange = {
    { sizeof(KSDATARANGE_AUDIO), 0, SIA_CAPTURE_FRAME, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    2, 16, 16, SIA_PCM_RATE, SIA_PCM_RATE
};
static PKSDATARANGE CaptureRanges[] = { (PKSDATARANGE)&CaptureRange };
static KSDATAFORMAT_WAVEFORMATEXTENSIBLE CaptureFormat = {
    { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, SIA_CAPTURE_FRAME, 0,
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    { { WAVE_FORMAT_EXTENSIBLE, 2, SIA_PCM_RATE, SIA_PCM_RATE * SIA_CAPTURE_FRAME, SIA_CAPTURE_FRAME, 16, 22 },
      {16}, KSAUDIO_SPEAKER_STEREO, STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM) }
};

static NTSTATUS Jack(PPCPROPERTY_REQUEST Request, ULONG Pin)
{
    if (Request->InstanceSize < sizeof(ULONG) || *(ULONG *)Request->Instance != Pin) return STATUS_INVALID_PARAMETER;
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

static NTSTATUS JackProperty(PPCPROPERTY_REQUEST Request) { return Jack(Request, 1); }
static NTSTATUS MicJackProperty(PPCPROPERTY_REQUEST Request) { return Jack(Request, 0); }
static PCPROPERTY_ITEM MicJackItems[] = {
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION,
      KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, MicJackProperty }
};
DEFINE_PCAUTOMATION_TABLE_PROP(MicJackAutomation, MicJackItems);
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

static PCPIN_DESCRIPTOR CapturePins[] = {
    {1, 1, 0, nullptr, {0, nullptr, 0, nullptr, 1, CaptureRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, AnalogRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static PCNODE_DESCRIPTOR CaptureNodes[] = {{0, nullptr, &KSNODETYPE_ADC, nullptr}};
static PCCONNECTION_DESCRIPTOR CaptureConnections[] = {
    {PCFILTER_NODE, 1, 0, 1}, {0, 0, PCFILTER_NODE, 0}
};
static PCFILTER_DESCRIPTOR CaptureFilter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), 2, CapturePins, sizeof(PCNODE_DESCRIPTOR), 1, CaptureNodes,
    2, CaptureConnections, 0, nullptr
};
static PCPIN_DESCRIPTOR MicPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, AnalogRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, AnalogRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static PCFILTER_DESCRIPTOR MicFilter = {
    0, &MicJackAutomation, sizeof(PCPIN_DESCRIPTOR), 2, MicPins, 0, 0, nullptr,
    1, TopologyConnections, 0, nullptr
};

class Stream final : public IMiniportWaveRTStreamNotification {
    LONG References = 1;
    PSIA_DEVICE Dev;
    PSIA_PCM Pcm;
    BOOLEAN Capture;
    PPORTWAVERTSTREAM Port;
    PMDL Mdl = nullptr;
    KSSTATE State = KSSTATE_STOP;
public:
    Stream(PSIA_DEVICE Device, PPORTWAVERTSTREAM StreamPort, BOOLEAN Input) : Dev(Device), Pcm(&Device->Stream[Input]), Capture(Input), Port(StreamPort) { Port->AddRef(); }
    ~Stream() {
        if (Mdl) FreeAudioBuffer(Mdl, Pcm->PcmSize);
        KIRQL Irql;
        KeAcquireSpinLock(&Pcm->NotifyLock, &Irql);
        Pcm->NotifyEvent = nullptr;
        KeReleaseSpinLock(&Pcm->NotifyLock, Irql);
        Port->Release();
        InterlockedExchange(&Pcm->StreamAllocated, 0);
    }
    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Object) override {
        return ::QueryInterface(this, Id, IID_IMiniportWaveRTStreamNotification, IID_IMiniportWaveRTStream, Object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&References); }
    STDMETHODIMP_(ULONG) Release() override { auto Count = InterlockedDecrement(&References); if (!Count) delete this; return Count; }
    STDMETHODIMP SetFormat(PKSDATAFORMAT Format) override { return State == KSSTATE_STOP ? CheckFormat(Format, Capture) : STATUS_INVALID_DEVICE_STATE; }
    STDMETHODIMP SetState(KSSTATE NewState) override {
        if (NewState > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
        NTSTATUS Status = STATUS_SUCCESS;
        if (NewState == KSSTATE_RUN) Status = SiaStreamStart(Dev, Capture);
        else if (State == KSSTATE_RUN) SiaStreamStop(Dev, Capture);
        if (NewState == KSSTATE_STOP) { Pcm->ReadOffset = 0; InterlockedExchange64(&Pcm->PlayedBytes, 0); }
        if (NT_SUCCESS(Status)) State = NewState;
        return Status;
    }
    STDMETHODIMP GetPosition(PKSAUDIO_POSITION Position) override {
        if (!Pcm->PcmSize) return STATUS_INVALID_DEVICE_STATE;
        if (!NT_SUCCESS(Pcm->Status)) return Pcm->Status;
        Position->PlayOffset = (ULONGLONG)InterlockedCompareExchange64(&Pcm->PlayedBytes, 0, 0) % Pcm->PcmSize;
        Position->WriteOffset = Capture ? Position->PlayOffset : Pcm->ReadOffset;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP AllocateAudioBuffer(ULONG Requested, PMDL *Buffer, ULONG *Actual, ULONG *Offset, MEMORY_CACHING_TYPE *Cache) override {
        return AllocateBufferWithNotification(0, Requested, Buffer, Actual, Offset, Cache);
    }
    STDMETHODIMP AllocateBufferWithNotification(ULONG Count, ULONG Requested, PMDL *Buffer,
        ULONG *Actual, ULONG *Offset, MEMORY_CACHING_TYPE *Cache) override {
        if (Mdl || !Requested || Count > 2 || Requested > 1024 * 1024) return STATUS_INVALID_PARAMETER;
        ULONG Unit = (Capture ? SIA_CAPTURE_BYTES : SIA_PCM_BYTES) * 2;
        ULONG Size = ((Requested + Unit - 1) / Unit) * Unit;
        if (Size <= 2 * Unit) Size = 3 * Unit;
        PHYSICAL_ADDRESS Highest;
        Highest.QuadPart = MAXLONGLONG;
        Mdl = Port->AllocatePagesForMdl(Highest, Size);
        if (!Mdl) return STATUS_INSUFFICIENT_RESOURCES;
        Pcm->Pcm = (PUCHAR)Port->MapAllocatedPages(Mdl, MmCached);
        if (!Pcm->Pcm) { Port->FreePagesFromMdl(Mdl); Mdl = nullptr; return STATUS_INSUFFICIENT_RESOURCES; }
        RtlZeroMemory(Pcm->Pcm, Size);
        Pcm->PcmSize = Size;
        Pcm->NotifyBytes = Count ? Size / Count : 0;
        *Buffer = Mdl;
        *Actual = Size;
        *Offset = 0;
        *Cache = MmCached;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(VOID) FreeAudioBuffer(PMDL Buffer, ULONG Size) override {
        UNREFERENCED_PARAMETER(Size);
        if (!Buffer || Buffer != Mdl) return;
        SiaStreamStop(Dev, Capture);
        Port->UnmapAllocatedPages(Pcm->Pcm, Mdl);
        Port->FreePagesFromMdl(Mdl);
        Pcm->Pcm = nullptr;
        Pcm->PcmSize = 0;
        Mdl = nullptr;
    }
    STDMETHODIMP_(VOID) FreeBufferWithNotification(PMDL Buffer, ULONG Size) override { FreeAudioBuffer(Buffer, Size); }
    STDMETHODIMP_(VOID) GetHWLatency(PKSRTAUDIO_HWLATENCY Latency) override {
        RtlZeroMemory(Latency, sizeof(*Latency));
        Latency->FifoSize = Capture ? 0 : 2 * SIA_PCM_BYTES;
    }
    STDMETHODIMP GetPositionRegister(PKSRTAUDIO_HWREGISTER Register) override { UNREFERENCED_PARAMETER(Register); return STATUS_NOT_SUPPORTED; }
    STDMETHODIMP GetClockRegister(PKSRTAUDIO_HWREGISTER Register) override { UNREFERENCED_PARAMETER(Register); return STATUS_NOT_SUPPORTED; }
    STDMETHODIMP RegisterNotificationEvent(PKEVENT Event) override {
        KIRQL Irql;
        KeAcquireSpinLock(&Pcm->NotifyLock, &Irql);
        NTSTATUS Status = Pcm->NotifyEvent && Pcm->NotifyEvent != Event ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
        if (NT_SUCCESS(Status)) Pcm->NotifyEvent = Event;
        KeReleaseSpinLock(&Pcm->NotifyLock, Irql);
        return Status;
    }
    STDMETHODIMP UnregisterNotificationEvent(PKEVENT Event) override {
        KIRQL Irql;
        KeAcquireSpinLock(&Pcm->NotifyLock, &Irql);
        if (Pcm->NotifyEvent == Event) Pcm->NotifyEvent = nullptr;
        KeReleaseSpinLock(&Pcm->NotifyLock, Irql);
        return STATUS_SUCCESS;
    }
};

class Wave final : public IMiniportWaveRT {
    LONG References = 1;
    PSIA_DEVICE Dev;
    BOOLEAN Capture;
public:
    Wave(PSIA_DEVICE Device, BOOLEAN Input) : Dev(Device), Capture(Input) {}
    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Object) override {
        return ::QueryInterface(this, Id, IID_IMiniportWaveRT, IID_IMiniport, Object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&References); }
    STDMETHODIMP_(ULONG) Release() override { auto Count = InterlockedDecrement(&References); if (!Count) delete this; return Count; }
    STDMETHODIMP Init(PUNKNOWN Adapter, PRESOURCELIST Resources, PPORTWAVERT Port) override {
        UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Resources); UNREFERENCED_PARAMETER(Port); return STATUS_SUCCESS;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *Description) override { *Description = Capture ? &CaptureFilter : &WaveFilter; return STATUS_SUCCESS; }
    STDMETHODIMP DataRangeIntersection(ULONG Pin, PKSDATARANGE Range, PKSDATARANGE Match,
        ULONG Size, PVOID Result, PULONG Length) override {
        UNREFERENCED_PARAMETER(Match);
        if (Pin != 0 || Range->FormatSize < sizeof(KSDATARANGE_AUDIO)) return STATUS_NO_MATCH;
        auto Audio = (PKSDATARANGE_AUDIO)Range;
        if (Audio->MaximumChannels < 2 || Audio->MinimumBitsPerSample > (Capture ? 16u : 24u) || Audio->MaximumBitsPerSample < (Capture ? 16u : 24u) ||
            Audio->MinimumSampleFrequency > SIA_PCM_RATE || Audio->MaximumSampleFrequency < SIA_PCM_RATE) return STATUS_NO_MATCH;
        *Length = sizeof(PcmFormat);
        if (!Size) return STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof(PcmFormat)) return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(Result, Capture ? &CaptureFormat : &PcmFormat, sizeof(PcmFormat)); return STATUS_SUCCESS;
    }
    STDMETHODIMP NewStream(PMINIPORTWAVERTSTREAM *Result, PPORTWAVERTSTREAM Port,
        ULONG Pin, BOOLEAN Input, PKSDATAFORMAT Format) override {
        *Result = nullptr;
        if (Pin || Input != Capture || !NT_SUCCESS(CheckFormat(Format, Capture))) return STATUS_INVALID_PARAMETER;
        if (InterlockedCompareExchange(&Dev->Stream[Capture].StreamAllocated, 1, 0)) return STATUS_DEVICE_BUSY;
        auto Created = new Stream(Dev, Port, Capture);
        if (!Created) { InterlockedExchange(&Dev->Stream[Capture].StreamAllocated, 0); return STATUS_INSUFFICIENT_RESOURCES; }
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
    BOOLEAN Capture;
public:
    explicit Topology(BOOLEAN Input) : Capture(Input) {}
    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Object) override {
        return ::QueryInterface(this, Id, IID_IMiniportTopology, IID_IMiniport, Object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&References); }
    STDMETHODIMP_(ULONG) Release() override { auto Count = InterlockedDecrement(&References); if (!Count) delete this; return Count; }
    STDMETHODIMP Init(PUNKNOWN Adapter, PRESOURCELIST Resources, PPORTTOPOLOGY Port) override {
        UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Resources); UNREFERENCED_PARAMETER(Port); return STATUS_SUCCESS;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *Description) override { *Description = Capture ? &MicFilter : &TopologyFilter; return STATUS_SUCCESS; }
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
    for (ULONG Index = 0; Index < 2 && NT_SUCCESS(Status); Index++) {
        auto Top = new Topology((BOOLEAN)Index);
        auto Audio = new Wave(Dev, (BOOLEAN)Index);
        if (!Top || !Audio) { if (Top) Top->Release(); if (Audio) Audio->Release(); Status = STATUS_INSUFFICIENT_RESOURCES; break; }
        PPORT TopPort = nullptr, WavePort = nullptr;
        Status = Install(Device, Irp, Resources, CLSID_PortTopology, Index ? L"TopologyMic" : L"Topology", Top, &TopPort);
        if (NT_SUCCESS(Status)) Status = Install(Device, Irp, Resources, CLSID_PortWaveRT, Index ? L"WaveMic" : L"Wave", Audio, &WavePort);
        else Audio->Release();
        if (NT_SUCCESS(Status)) Status = Index ? PcRegisterPhysicalConnection(Device, TopPort, 1, WavePort, 1) :
            PcRegisterPhysicalConnection(Device, WavePort, 1, TopPort, 0);
        if (TopPort) TopPort->Release();
        if (WavePort) WavePort->Release();
    }
    if (!NT_SUCCESS(Status)) SiaHardwareStop(Dev);
    return Status;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Device)
{
    return PcAddAdapterDevice(Driver, Device, StartDevice, 4, sizeof(Extension));
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
