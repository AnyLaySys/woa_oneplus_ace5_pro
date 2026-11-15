param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot '../wddm/dpu.c'))
$first=$source.IndexOf('typedef struct { ULONG Base,OriginalAddress,OriginalStride,Offset; } DPU_PIPE;')
$last=$source.IndexOf('static VOID KickDpu(A830_ADAPTER *a) {',$first)
if($first-lt 0 -or $last-lt $first){throw 'DPU production struct extraction failed'}
$declarations=@'
static ULONG Read(A830_DPU *,ULONG);
static void Write(A830_DPU *,ULONG,ULONG);
static ULONG Frame(A830_DPU *);
static ULONG TxFrame(A830_DPU *);
static BOOLEAN FlushComplete(A830_DPU *);
static void TraceFlip(A830_DPU *,ULONG,ULONGLONG,ULONGLONG,ULONGLONG,ULONGLONG,ULONG);
static void A830WakeDisplay(A830_ADAPTER *);
'@
$body=$source.Substring($first,$last-$first)+$declarations+"`n"
$first=$source.IndexOf('static VOID RestoreTransaction(')
$last=$source.IndexOf('static BOOLEAN Notify(',$first)
if($first-lt 0 -or $last-lt $first){throw 'DPU production transaction extraction failed'}
$body+=$source.Substring($first,$last-$first)
$first=$source.IndexOf('NTSTATUS A830DpuQueue(')
$last=$source.IndexOf('VOID A830DpuEnableVsync(',$first)
if($first-lt 0 -or $last-lt $first){throw 'DPU production Queue extraction failed'}
$body+=$source.Substring($first,$last-$first)
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'dpu-transaction-under-test.inc'),$body)
