#include "scheduler.h"
#include <stdexcept>
#include <string>
#include <cstring>
#include <dxcore.h>
#include <dxcore_interface.h>
#include <wrl/client.h>
#include <objbase.h>
#pragma comment(lib, "dxcore.lib")
#pragma comment(lib, "ole32.lib")
static void CheckStatus(NTSTATUS status, const char* operation) {
    if (status < 0) throw std::runtime_error(std::string(operation) + ": NTSTATUS " + std::to_string(static_cast<uint32_t>(status)));
}
NpuScheduler::NpuScheduler() {
    try {
        Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> factory;
        HRESULT result = DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory));
        if (FAILED(result)) throw std::runtime_error("Cannot create DXCore adapter factory");
        const GUID npuAttribute = {0xd46140c4,0xadd7,0x451b,{0x9e,0x56,0x06,0xfe,0x8c,0x3b,0x58,0xed}};
        // Hardware-type attributes also discover NPUs without a Direct3D UMD.
        Microsoft::WRL::ComPtr<IDXCoreAdapterList> list;
        result = factory->CreateAdapterList(1, &npuAttribute, IID_PPV_ARGS(&list));
        if (FAILED(result)) throw std::runtime_error("Cannot enumerate DXCore NPU hardware");
        for (uint32_t i = 0; i < list->GetAdapterCount(); i++) {
            Microsoft::WRL::ComPtr<IDXCoreAdapter> hardware;
            if (FAILED(list->GetAdapter(i, IID_PPV_ARGS(&hardware)))) continue;
            LUID luid{};
            if (FAILED(hardware->GetProperty(DXCoreAdapterProperty::InstanceLuid, &luid))) continue;
            D3DKMT_OPENADAPTERFROMLUID opened{}; opened.AdapterLuid = luid;
            if (D3DKMTOpenAdapterFromLuid(&opened) < 0) continue;
            NPU_SCHEDULER_INFO info{}; D3DKMT_QUERYADAPTERINFO query{};
            query.hAdapter = opened.hAdapter; query.Type = KMTQAITYPE_UMDRIVERPRIVATE; query.pPrivateDriverData = &info; query.PrivateDriverDataSize = sizeof(info);
            if (!adapter_ && D3DKMTQueryAdapterInfo(&query) >= 0 && info.Magic == NPU_SCHEDULER_MAGIC && info.Version == NPU_SCHEDULER_ABI && info.RuntimeVersion == QCOM_NPU_VERSION)
                adapter_ = opened.hAdapter;
            else { D3DKMT_CLOSEADAPTER close{opened.hAdapter}; D3DKMTCloseAdapter(&close); }
        }
        if (!adapter_) throw std::runtime_error("The matching Qualcomm NPU MCDM driver is unavailable");
        D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter_;
        CheckStatus(D3DKMTCreateDevice(&device), "Create NPU MCDM device"); device_ = device.hDevice;
        completion_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completion_) throw std::runtime_error("Cannot create NPU completion event");
        D3DKMT_CREATEPAGINGQUEUE paging{}; paging.hDevice = device_;
        CheckStatus(D3DKMTCreatePagingQueue(&paging), "Create NPU paging queue");
        paging_ = paging.hPagingQueue; pagingFence_ = paging.hSyncObject;
        GUID cookie{}; if (FAILED(CoCreateGuid(&cookie))) throw std::runtime_error("Cannot identify NPU memory ownership");
        std::memcpy(&cookie_, &cookie, sizeof(cookie_));
        NPU_SCHEDULER_CONTEXT data{NPU_SCHEDULER_MAGIC, 2, reinterpret_cast<uint64_t>(completion_), cookie_};
        D3DKMT_CREATECONTEXTVIRTUAL context{}; context.hDevice = device_; context.NodeOrdinal = 0; context.EngineAffinity = 1;
        context.pPrivateDriverData = &data; context.PrivateDriverDataSize = sizeof(data); context.ClientHint = D3DKMT_CLIENTHINT_QNN;
        CheckStatus(D3DKMTCreateContextVirtual(&context), "Create NPU MCDM context"); context_ = context.hContext;
        command_ = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!command_) throw std::runtime_error("Cannot allocate NPU command buffer");
        uint32_t magic = NPU_SCHEDULER_MAGIC;
        D3DDDI_ALLOCATIONINFO info{}; info.pSystemMem = command_; info.pPrivateDriverData = &magic; info.PrivateDriverDataSize = sizeof(magic);
        D3DKMT_CREATEALLOCATION create{}; create.hDevice = device_; create.NumAllocations = 1; create.pAllocationInfo = &info;
        CheckStatus(D3DKMTCreateAllocation(&create), "Create NPU command buffer"); commandAllocation_ = info.hAllocation;
        commandAddress_ = MapMemory(commandAllocation_, 4096);
    } catch (...) { Close(); throw; }
}
void NpuScheduler::Close() {
    if (context_) { D3DKMT_DESTROYCONTEXT context{context_}; D3DKMTDestroyContext(&context); context_ = 0; }
    if (commandAllocation_) { FreeMemory({commandAllocation_, commandAddress_, 4096}); commandAllocation_ = 0; commandAddress_ = 0; }
    if (command_) { VirtualFree(command_, 0, MEM_RELEASE); command_ = nullptr; }
    for (const auto& item : memory_) FreeMemory(item.second);
    memory_.clear();
    if (paging_) { D3DDDI_DESTROYPAGINGQUEUE paging{paging_}; D3DKMTDestroyPagingQueue(&paging); paging_ = pagingFence_ = 0; }
    if (completion_) { CloseHandle(completion_); completion_ = nullptr; }
    if (device_) { D3DKMT_DESTROYDEVICE device{device_}; D3DKMTDestroyDevice(&device); device_ = 0; }
    if (adapter_) { D3DKMT_CLOSEADAPTER adapter{adapter_}; D3DKMTCloseAdapter(&adapter); adapter_ = 0; }
}
NpuScheduler::~NpuScheduler() { Close(); }
void NpuScheduler::Submit(const QCOM_NPU_TICKET& ticket) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (ticket.Abi != 1 || ticket.Reserved || !ticket.Token || !command_) throw std::runtime_error("Invalid queued NPU execution");
    NPU_SCHEDULER_COMMAND command{NPU_SCHEDULER_MAGIC, 1, ticket.Token}; ResetEvent(completion_);
    std::memcpy(command_, &command, sizeof(command));
    MemoryBarrier();
    D3DKMT_SUBMITCOMMAND submit{}; submit.Commands = commandAddress_; submit.CommandLength = sizeof(command);
    submit.BroadcastContextCount = 1; submit.BroadcastContext[0] = context_;
    submit.pPrivateDriverData = &command; submit.PrivateDriverDataSize = sizeof(command);
    CheckStatus(D3DKMTSubmitCommand(&submit), "Submit NPU work to Windows scheduler");
    if (WaitForSingleObject(completion_, 30000) != WAIT_OBJECT_0) throw std::runtime_error("NPU MCDM execution completion timed out");
}
void NpuScheduler::RegisterMemory(const QCOM_NPU_SHARED_ALLOCATION& allocation) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto& a = allocation.Allocation;
    NPU_SCHEDULER_MEMORY data{NPU_SCHEDULER_MAGIC, a.Id, a.Bytes, GetCurrentProcessId(), allocation.UserAddress, cookie_};
    D3DDDI_ALLOCATIONINFO2 info{}; info.pSystemMem = reinterpret_cast<void*>(allocation.UserAddress);
    info.pPrivateDriverData = &data; info.PrivateDriverDataSize = sizeof(data);
    D3DKMT_CREATEALLOCATION create{}; create.hDevice = device_; create.NumAllocations = 1; create.pAllocationInfo2 = &info;
    NTSTATUS created = D3DKMTCreateAllocation2(&create);
    if (created == static_cast<NTSTATUS>(0xc00000bbu)) {
        D3DDDI_ALLOCATIONINFO legacy{}; legacy.pSystemMem = info.pSystemMem;
        legacy.pPrivateDriverData = &data; legacy.PrivateDriverDataSize = sizeof(data);
        create.pAllocationInfo = &legacy; created = D3DKMTCreateAllocation(&create); info.hAllocation = legacy.hAllocation;
    }
    CheckStatus(created, "Register NPU system memory with Windows");
    try {
        uint64_t address = MapMemory(info.hAllocation, a.Bytes);
        memory_.emplace(a.Id, Memory{info.hAllocation, address, a.Bytes});
    } catch (...) {
        D3DKMT_DESTROYALLOCATION destroy{device_, 0, &info.hAllocation, 1}; D3DKMTDestroyAllocation(&destroy);
        throw;
    }
}
void NpuScheduler::WaitPaging(uint64_t fence) {
    if (fence) {
        ResetEvent(completion_);
        D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{}; wait.hDevice = device_; wait.ObjectCount = 1;
        wait.ObjectHandleArray = &pagingFence_; wait.FenceValueArray = &fence; wait.hAsyncEvent = completion_;
        CheckStatus(D3DKMTWaitForSynchronizationObjectFromCpu(&wait), "Wait for NPU memory residency");
        if (WaitForSingleObject(completion_, 30000) != WAIT_OBJECT_0) throw std::runtime_error("NPU memory residency timed out");
    }
}
uint64_t NpuScheduler::MapMemory(D3DKMT_HANDLE allocation, uint32_t bytes) {
    D3DDDI_MAKERESIDENT resident{}; resident.hPagingQueue = paging_;
    resident.NumAllocations = 1; resident.AllocationList = &allocation;
    CheckStatus(D3DKMTMakeResident(&resident), "Make NPU memory resident"); WaitPaging(resident.PagingFenceValue);
    D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hPagingQueue = paging_; map.hAllocation = allocation;
    map.SizeInPages = bytes / 4096; map.Protection.Write = 1; map.Protection.Execute = allocation == commandAllocation_;
    CheckStatus(D3DKMTMapGpuVirtualAddress(&map), "Map NPU process address space");
    try { WaitPaging(map.PagingFenceValue); }
    catch (...) { D3DKMT_FREEGPUVIRTUALADDRESS free{adapter_, map.VirtualAddress, bytes}; D3DKMTFreeGpuVirtualAddress(&free); throw; }
    return map.VirtualAddress;
}
void NpuScheduler::FreeMemory(const Memory& memory) {
    if (memory.Address) { D3DKMT_FREEGPUVIRTUALADDRESS free{adapter_, memory.Address, memory.Bytes}; D3DKMTFreeGpuVirtualAddress(&free); }
    D3DKMT_DESTROYALLOCATION destroy{device_, 0, &memory.Handle, 1}; D3DKMTDestroyAllocation(&destroy);
}
void NpuScheduler::UnregisterMemory(uint32_t id) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = memory_.find(id); if (it == memory_.end()) return;
    FreeMemory(it->second);
    memory_.erase(it);
}
