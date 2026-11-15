#include "../gpu.h"
#include "../mmu-table.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    GPU_VM vm={0};vm.Tables.Size=GPU_TABLE_BYTES;vm.Tables.Physical=0x10000000;
    vm.Tables.Cpu=calloc(1,vm.Tables.Size);vm.Used=1;
    if(!vm.Tables.Cpu)return 1;
    for(ULONGLONG address=0;address<GPU_VA_END;address+=0x200000) {
        PULONGLONG first=Entry(&vm,address,TRUE),last=Entry(&vm,address+0x1FF000,TRUE);
        if(!first || !last || first==last)return 1;
        *first=address|0xE43;*last=(address+0x1FF000)|0xE43;
    }
    ULONG used=vm.Used;
    for(ULONGLONG address=0;address<GPU_VA_END;address+=0x200000) {
        PULONGLONG first=Entry(&vm,address,FALSE),last=Entry(&vm,address+0x1FF000,FALSE);
        if(!first || !last || *first!=(address|0xE43) || *last!=((address+0x1FF000)|0xE43))return 1;
        *first=*last=0;
        if(!Entry(&vm,address,TRUE) || vm.Used!=used)return 1;
    }
    if(used!=8209)return 1;
    printf("{\"full_16gb_page_tables\":\"PASS\",\"table_pages\":%lu,\"reuse\":\"PASS\"}\n",used);
    free(vm.Tables.Cpu);return 0;
}
