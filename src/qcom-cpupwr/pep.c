#include "cpupower.h"

static LONG FindCore(PCUNICODE_STRING id) {
    static const PCWSTR names[]={L"\\_SB.CPU",L"\\_SB_.CPU",L"ACPI\\QUALCOMM_-_ARMV8_(64-BIT)_FAMILY_8_MODEL_1_REVISION_404\\"};
    if(!id || !id->Buffer)return -1;
    for(ULONG i=0;i<RTL_NUMBER_OF(names);i++) {
        UNICODE_STRING prefix;RtlInitUnicodeString(&prefix,names[i]);
        if(id->Length==prefix.Length+sizeof(WCHAR) && RtlPrefixUnicodeString(&prefix,id,TRUE)) {
            WCHAR number=id->Buffer[prefix.Length/sizeof(WCHAR)];
            if(number>=L'0' && number<=L'7')return (LONG)(number-L'0');
        }
    }
    return -1;
}
static SM_CORE *CoreHandle(PEPHANDLE handle) {
    ULONG_PTR offset=(ULONG_PTR)handle-(ULONG_PTR)Sm.Core;
    return offset<sizeof(Sm.Core) && offset%sizeof(SM_CORE)==0?(SM_CORE*)handle:NULL;
}

BOOLEAN SmDeviceNotification(ULONG notification,PVOID data) {
    LONG core;
    switch(notification) {
    case PEP_DPM_PREPARE_DEVICE:case PEP_DPM_ABANDON_DEVICE: {
        PPEP_PREPARE_DEVICE prepare=data;
        core=FindCore(prepare->DeviceId);
        prepare->DeviceAccepted=(core>=0);
        /* Recognition of the notification is separate from ownership. */
        return TRUE;
    }
    case PEP_DPM_REGISTER_DEVICE: {
        PPEP_REGISTER_DEVICE registration=data;
        core=FindCore(registration->DeviceId);
        if(core<0) {
            registration->DeviceHandle=NULL;
            registration->DeviceAccepted=PepDeviceNotAccepted;
            return TRUE;
        }
        registration->DeviceHandle=(PEPHANDLE)&Sm.Core[core];
        registration->DeviceAccepted=PepDeviceAccepted;
        InterlockedOr(&Sm.RegisteredMask,1<<core);
        if(Sm.RegisteredMask==255)KeSetEvent(&Sm.Wake,0,FALSE);
        return TRUE;
    }
    case PEP_DPM_UNREGISTER_DEVICE: {
        PPEP_UNREGISTER_DEVICE registration=data;
        SM_CORE *item=CoreHandle(registration->DeviceHandle);
        if(item)InterlockedAnd(&Sm.RegisteredMask,~(1<<item->Number));
        return TRUE;
    }
    case PEP_DPM_DEVICE_POWER_STATE: {
        PPEP_DEVICE_POWER_STATE power=data;
        if(!CoreHandle(power->DeviceHandle))return FALSE;
        power->Complete=TRUE;return TRUE;
    }
    case PEP_DPM_DEVICE_STARTED:return TRUE;
    case PEP_DPM_NOTIFY_COMPONENT_IDLE_STATE: {
        PPEP_NOTIFY_COMPONENT_IDLE_STATE state=data;
        if(state->IdleState!=0)return FALSE;
        state->Completed=TRUE;return TRUE;
    }
    case PEP_DPM_SYSTEM_LATENCY_UPDATE:
        /* The OS owns idle-state selection; acknowledge this legacy notice. */
        return TRUE;
    case PEP_DPM_WORK: {
        PPEP_WORK work=data;
        work->WorkInformation=NULL;work->NeedWork=FALSE;
        return TRUE;
    }
    case PEP_DPM_POWER_CONTROL_REQUEST: {
        PPEP_POWER_CONTROL_REQUEST request=data;
        request->Status=STATUS_NOT_SUPPORTED;request->BytesReturned=0;
        return TRUE;
    }
    case PEP_DPM_POWER_CONTROL_COMPLETE:return TRUE;
    default:return FALSE;
    }
}

BOOLEAN SmProcessorNotification(PEPHANDLE handle,ULONG notification,PVOID data) {
    SM_CORE *core=CoreHandle(handle);ULONG domain,index,i;
    /* Platform queries are not addressed to an individual processor. */
    if(notification==PEP_NOTIFY_PPM_QUERY_PLATFORM_STATES) {
        if(!data)return FALSE;
        ((PPEP_PPM_QUERY_PLATFORM_STATES)data)->PlatformStateCount=0;
        return TRUE;
    }
    if(notification==PEP_NOTIFY_PPM_PERF_CHECK_COMPLETE) {
        if(KeQueryInterruptTime()-Sm.LastSample>=DVFS_SAMPLE_100NS)SmWake();
        return TRUE;
    }
    if(notification==PEP_NOTIFY_PPM_QUERY_DOMAIN_INFO) {
        PPEP_PPM_QUERY_DOMAIN_INFO information=data;
        if(!information || information->DomainId>=2)return FALSE;
        information->CoordinationType=PROCESSOR_DOMAIN_COORDINATION_SW_ANY;
        information->IdleProcessorsDiscounted=TRUE;
        information->SchedulerDirectedTransitionsSupported=FALSE;
        information->AffinitizePerfSet=FALSE;
        information->WorstCaseTransitionLatency=2000000;
        information->WorstCaseTransitionOverhead=1000000;
        return TRUE;
    }
    if(!core)return FALSE;
    domain=SmDomain(core->Number);
    switch(notification) {
    case PEP_NOTIFY_PPM_QUERY_CAPABILITIES: {
        PPEP_PPM_QUERY_CAPABILITIES capabilities=data;
        RtlZeroMemory(capabilities,sizeof(*capabilities));
        capabilities->FeedbackCounterCount=2;
        capabilities->IdleStateCount=2;
        capabilities->PerformanceStatesSupported=TRUE;
        capabilities->DiscretePerformanceStateCount=SM_LEVEL_COUNT;
        return TRUE;
    }
    case PEP_NOTIFY_PPM_QUERY_FEEDBACK_COUNTERS: {
        PPEP_PPM_QUERY_FEEDBACK_COUNTERS counters=data;
        if(counters->Count!=2)return FALSE;
        RtlZeroMemory(counters->Counters,2*sizeof(counters->Counters[0]));
        for(i=0;i<2;i++) {
            counters->Counters[i].Type=PROCESSOR_FEEDBACK_TYPE_INSTANTANEOUS;
            counters->Counters[i].Counter=i;
            counters->Counters[i].NominalRate=i?Sm.NominalKhz[domain]:(Sm.NominalKhz[domain]+500)/1000;
        }
        return TRUE;
    }
    case PEP_NOTIFY_PPM_FEEDBACK_READ: {
        PPEP_PPM_FEEDBACK_READ read=data;
        LONG applied=InterlockedCompareExchange(&Sm.Applied[domain],0,0);
        if(read->CounterIndex>=2 || applied<0 || applied>=SM_LEVEL_COUNT)return FALSE;
        ULONG khz=SmFrequencyKhz[domain][applied];
        /* Read the firmware-acknowledged operating point from any processor.
         * No PMU programming, affinity changes, stalls or hardware polling. */
        read->InstantaneousValue=read->CounterIndex?khz:(khz+500)/1000;
        return TRUE;
    }
    case PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES: {
        PPEP_PPM_QUERY_PERF_CAPABILITIES capabilities=data;
        capabilities->HighestPerformance=SmFrequencyKhz[domain][SM_LEVEL_COUNT-1];
        capabilities->NominalPerformance=Sm.NominalKhz[domain];
        capabilities->LowestNonlinearPerformance=SmFrequencyKhz[domain][0];
        capabilities->LowestPerformance=capabilities->LowestNonlinearPerformance;
        capabilities->DomainId=domain;capabilities->DomainMembers=domain?2:6;
        return TRUE;
    }
    case PEP_NOTIFY_PPM_QUERY_DISCRETE_PERF_STATES: {
        PPEP_PPM_QUERY_DISCRETE_PERF_STATES states=data;
        if(states->Count!=SM_LEVEL_COUNT || !states->States)return FALSE;
        RtlZeroMemory(states->States,states->Count*sizeof(states->States[0]));
        for(i=0;i<states->Count;i++) {
            index=SmLevelFromState(i);
            states->States[i].Performance=SmFrequencyKhz[domain][index];
            states->States[i].Frequency=(SmFrequencyKhz[domain][index]+500)/1000;
        }
        return TRUE;
    }
    case PEP_NOTIFY_PPM_PERF_CONSTRAINTS: {
        PPEP_PPM_PERF_CONSTRAINTS constraints=data;
        constraints->GuaranteedPerformanceLimit=min(Sm.NominalKhz[domain],SmFrequencyKhz[domain][SmEffectiveCeiling(domain)]);
        constraints->LimitReasons=Sm.Fault?PERFORMANCE_LIMIT_POWER:
            (Sm.CoolingBand[domain] || Sm.SweetBand[domain] || Sm.TemperatureAverage[domain]==MINLONG)?PERFORMANCE_LIMIT_THERMAL:0;
        return TRUE;
    }
    case PEP_NOTIFY_PPM_PERF_SET: {
        PPEP_PPM_PERF_SET request=data;
        index=SmChooseLevel(domain,request->DesiredPerformance,request->MaximumPerformance);
        SmRequestLevel(domain,index);return TRUE;
    }
    case PEP_NOTIFY_PPM_PERF_SET_STATE: {
        PPEP_PPM_PERF_SET_STATE request=data;
        if(request->DesiredPerformanceState==PEP_SET_STATE_AUTONOMOUS_MODE)return FALSE;
        index=SmChooseState(request->DesiredPerformanceState,
                            request->MinimumPerformanceState,request->MaximumPerformanceState);
        if(index==MAXULONG)return FALSE;
        SmRequestLevel(domain,index);return TRUE;
    }
    case PEP_NOTIFY_PPM_QUERY_PERF_EPP_SUPPORT:
        ((PPEP_PPM_QUERY_PERF_EPP_SUPPORT)data)->EnergyPerformancePreferenceSupported=FALSE;
        return TRUE;
    case PEP_NOTIFY_PPM_QUERY_AUTONOMOUS_SUPPORT:
        ((PPEP_PPM_QUERY_PERF_AUTONOMOUS_SUPPORT)data)->AutonomousCapability=PROCESSOR_AUTONOMOUS_CAPABILITY_UNSUPPORTED;
        return TRUE;
    case PEP_NOTIFY_PPM_QUERY_IDLE_STATES_V2: {
        PPEP_PPM_QUERY_IDLE_STATES_V2 states=data;
        if(states->Count!=2)return FALSE;
        RtlZeroMemory(states->IdleStates,2*sizeof(states->IdleStates[0]));
        for(i=0;i<2;i++) {
            states->IdleStates[i].Interruptible=TRUE;
            states->IdleStates[i].CacheCoherent=TRUE;
            states->IdleStates[i].ThreadContextRetained=TRUE;
            states->IdleStates[i].WakesSpuriously=TRUE;
        }
        states->IdleStates[0].Latency=10;states->IdleStates[0].BreakEvenDuration=10;
        states->IdleStates[1].Latency=domain?3020:2220;
        states->IdleStates[1].BreakEvenDuration=domain?6860:5600;
        return TRUE;
    }
    case PEP_NOTIFY_PPM_TEST_IDLE_STATE: {
        PPEP_PPM_TEST_IDLE_STATE state=data;
        state->VetoReason=state->ProcessorState<2 && (!state->ProcessorState || Sm.Retention) &&
                          state->PlatformState==PEP_PLATFORM_IDLE_STATE_NONE?0:1;
        return TRUE;
    }
    case PEP_NOTIFY_PPM_IDLE_COMPLETE:
        /* Context is retained: acknowledge completion without a restore step.
         * fxppm treats rejection of this required notification as bugcheck 143. */
        return TRUE;
    case PEP_NOTIFY_PPM_IDLE_PRE_EXECUTE:
    case PEP_NOTIFY_PPM_IDLE_EXECUTE: {
        PPEP_PPM_IDLE_EXECUTE state=data;
        if(state->ProcessorState>=2 || state->PlatformState!=PEP_PLATFORM_IDLE_STATE_NONE ||
           KeGetCurrentProcessorNumber()!=core->Number){state->Status=STATUS_INVALID_PARAMETER;return TRUE;}
        state->Status=STATUS_SUCCESS;
        if(notification==PEP_NOTIFY_PPM_IDLE_PRE_EXECUTE)return TRUE;
        __dsb(_ARM64_BARRIER_SY);
        /* SM8750 DT retention state 4 keeps CPU context, caches and timers.
         * Firmware rejection permanently falls back to WFI for this boot. */
        if(!state->ProcessorState || !Sm.Retention)__wfi();
        else if(SmPsci(SM_CPU_SUSPEND,4)<0){InterlockedExchange(&Sm.Retention,0);__wfi();}
        return TRUE;
    }
    default:return FALSE;
    }
}
