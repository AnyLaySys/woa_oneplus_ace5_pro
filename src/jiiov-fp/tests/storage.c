#include "fingerprint.h"
typedef struct { UCHAR Data[65536]; ULONG Size,Position; BOOLEAN Exists; ULONG Open; } FILE_MOCK;
static FILE_MOCK Files[2];
static FILE_MOCK Directory;
static ULONG Opens;
static ULONG Word(const UCHAR *b,ULONG o) {ULONG n;memcpy(&n,b+o,4);return n;}
static VOID Put(UCHAR *b,ULONG o,ULONG n) {memcpy(b+o,&n,4);}
NTSTATUS ZwCreateFile(HANDLE *h,ACCESS_MASK access,OBJECT_ATTRIBUTES *a,IO_STATUS_BLOCK *io,PVOID allocation,ULONG attrs,ULONG share,ULONG disposition,ULONG options,PVOID ea,ULONG len) {
    (void)access;(void)io;(void)allocation;(void)attrs;(void)share;(void)options;(void)ea;(void)len;
    Opens++;
    FILE_MOCK *f=(options&FILE_DIRECTORY_FILE)?&Directory:&Files[wcsstr(a->ObjectName->Buffer,L".normal")?1:0];
    if(disposition==FILE_OPEN&&!f->Exists)return STATUS_OBJECT_NAME_NOT_FOUND;
    f->Exists=TRUE;f->Position=0;if(disposition==FILE_OVERWRITE_IF)f->Size=0;f->Open++;*h=f;return STATUS_SUCCESS;
}
NTSTATUS ZwReadFile(HANDLE h,PVOID e,PVOID apc,PVOID context,IO_STATUS_BLOCK *io,PVOID b,ULONG n,LARGE_INTEGER *off,PVOID key) {
    (void)e;(void)apc;(void)context;(void)key;FILE_MOCK *f=h;ULONG p=off?(ULONG)off->QuadPart:f->Position;
    if(p>=f->Size){io->Information=0;return STATUS_END_OF_FILE;}
    n=min(n,f->Size-p);memcpy(b,f->Data+p,n);io->Information=n;if(!off)f->Position=p+n;return STATUS_SUCCESS;
}
NTSTATUS ZwWriteFile(HANDLE h,PVOID e,PVOID apc,PVOID context,IO_STATUS_BLOCK *io,PVOID b,ULONG n,LARGE_INTEGER *off,PVOID key) {
    (void)e;(void)apc;(void)context;(void)key;FILE_MOCK *f=h;ULONG p=off?(ULONG)off->QuadPart:f->Position;
    assert(p<=sizeof(f->Data)&&n<=sizeof(f->Data)-p);memcpy(f->Data+p,b,n);if(p+n>f->Size)f->Size=p+n;io->Information=n;if(!off)f->Position=p+n;return STATUS_SUCCESS;
}
NTSTATUS ZwSetInformationFile(HANDLE h,IO_STATUS_BLOCK *io,PVOID b,ULONG n,ULONG cls) {
    (void)io;(void)n;FILE_MOCK *f=h;if(cls==FileEndOfFileInformation)f->Size=(ULONG)((FILE_END_OF_FILE_INFORMATION *)b)->EndOfFile.QuadPart;else {assert(cls==FileDispositionInformation);f->Exists=FALSE;}return STATUS_SUCCESS;
}
NTSTATUS ZwQueryInformationFile(HANDLE h,IO_STATUS_BLOCK *io,PVOID b,ULONG n,ULONG cls) {
    (void)io;(void)n;assert(cls==FileStandardInformation);FILE_MOCK *f=h;FILE_STANDARD_INFORMATION *s=b;s->EndOfFile.QuadPart=f->Size;s->AllocationSize.QuadPart=4096;s->NumberOfLinks=1;return STATUS_SUCCESS;
}
VOID ZwClose(HANDLE h) {FILE_MOCK *f=h;assert(f->Open);f->Open--;}
static VOID Request(JFP_TEE *t,BOOLEAN normal,ULONG cmd,const CHAR *name) {
    UCHAR *b=t->Memory+(normal?t->FileOffset:t->StorageOffset);memset(b,0,normal?t->FileSize:t->StorageSize);Put(b,0,cmd);if(name)strcpy((CHAR *)b+4,name);
}
int main(void) {
    assert(!JfpQteeStoragePrepare() && Opens==2 && !Directory.Open);
    JFP_TEE t={0};t.StorageSize=131072;t.FileOffset=t.StorageSize;t.FileSize=32768;t.Memory=calloc(1,t.StorageSize+t.FileSize);assert(t.Memory);
    UCHAR *gp=t.Memory,*fs=t.Memory+t.FileOffset;const CHAR *path="/mnt/vendor/persist/data/jv_opt_data.bin",*normal="/mnt/vendor/persist/data/jv_opt_data.bin.normal";
    Request(&t,FALSE,12,NULL);assert(!JfpQteeStorage(&t,FALSE)&&Word(gp,4)==2&&!Word(gp,8));
    Request(&t,FALSE,1,path);Put(gp,0x108,381);for(ULONG i=0;i<381;i++)gp[0x110+i]=(UCHAR)i;
    assert(!JfpQteeStorage(&t,FALSE)&&!Word(gp,4)&&Word(gp,8)==381&&Files[0].Size==381);
    Request(&t,FALSE,0,path);Put(gp,0x108,381);assert(!JfpQteeStorage(&t,FALSE)&&Word(gp,8)==381);for(ULONG i=0;i<381;i++)assert(gp[12+i]==(UCHAR)i);
    ULONG before=Opens;Request(&t,FALSE,1,path);Put(gp,0x108,UINT32_MAX);assert(!JfpQteeStorage(&t,FALSE)&&Word(gp,4)==22&&Opens==before);
    Request(&t,FALSE,1,"/mnt/vendor/persist/data/../x");assert(!JfpQteeStorage(&t,FALSE)&&Word(gp,4)==13&&Opens==before);
    Request(&t,FALSE,1,"C:/x");assert(!JfpQteeStorage(&t,FALSE)&&Word(gp,4)==13&&Opens==before);
    Request(&t,TRUE,0x202,normal);Put(fs,0x104,0x41);assert(!JfpQteeStorage(&t,TRUE));ULONG fd=Word(fs,4);assert(fd==1);
    Request(&t,TRUE,0x208,NULL);Put(fs,4,fd);Put(fs,0x4e28,329);memset(fs+8,0x52,329);assert(!JfpQteeStorage(&t,TRUE)&&Word(fs,4)==329);
    Request(&t,TRUE,0x209,NULL);Put(fs,4,fd);assert(!JfpQteeStorage(&t,TRUE)&&!Word(fs,4));assert(!Files[1].Open&&Files[0].Size==381&&Files[1].Size==329);
    Request(&t,TRUE,0x20f,normal);assert(!JfpQteeStorage(&t,TRUE)&&!Word(fs,100)&&Word(fs,48)==329);
    Request(&t,TRUE,0x202,normal);assert(!JfpQteeStorage(&t,TRUE)&&Word(fs,4)==fd);
    Request(&t,TRUE,0x207,NULL);Put(fs,4,fd);Put(fs,8,400);assert(!JfpQteeStorage(&t,TRUE)&&Word(fs,0x4e24)==329);for(ULONG i=0;i<329;i++)assert(fs[4+i]==0x52);
    Request(&t,TRUE,0x207,NULL);Put(fs,4,fd);Put(fs,8,400);assert(!JfpQteeStorage(&t,TRUE)&&!Word(fs,0x4e24));
    Request(&t,TRUE,0x208,NULL);Put(fs,4,fd);Put(fs,0x4e28,UINT32_MAX);assert(!JfpQteeStorage(&t,TRUE)&&Word(fs,4)==UINT32_MAX);
    JfpQteeStorageClose(&t);assert(!Files[0].Open&&!Files[1].Open&&!t.Files[0]);free(t.Memory);
    puts("GP and normal file ABI, ciphertext preservation, bounds, path confinement and handle cleanup passed");return 0;
}
