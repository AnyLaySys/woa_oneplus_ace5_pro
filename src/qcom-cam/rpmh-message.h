#pragma once

static ULONG CamRpmhMessage(BOOLEAN write) {return 8u|0x100u|(write?0x10000u:0);}

static BOOLEAN CamRpmhSameResource(ULONG first,ULONG second) {
    return first==second||(((first>>16)&15)==4&&((first^second)&0xfffffff0u)==0);
}
