/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
static USHORT Jle16(const UCHAR *P) { return (USHORT)(P[0] | (P[1]<<8)); }
static ULONG Jle32(const UCHAR *P) { ULONG V; RtlCopyMemory(&V,P,4); return V; }
static USHORT Jbe16(const UCHAR *P) { return (USHORT)((P[0]<<8)|P[1]); }
static VOID Jputle16(UCHAR *P, ULONG V) { P[0]=(UCHAR)V;P[1]=(UCHAR)(V>>8); }
static BOOLEAN Jeq(const UCHAR *A,const UCHAR *B,ULONG N) { ULONG I;UCHAR V=0;for(I=0;I<N;I++)V|=A[I]^B[I];return V==0; }
static ULONG Jtlv(ULONG T,ULONG N) { return (T<<16)|N; }
static BOOLEAN Jtlvs(const UCHAR *P,ULONG N) { ULONG L;while(N){if(N<4)return FALSE;L=Jle32(P)&65535;if(L>N-4)return FALSE;N-=L+4;P+=L+4;}return TRUE; }
static const UCHAR *Jfind(const UCHAR *P,ULONG N,ULONG T,ULONG *L) { ULONG X;*L=0;while(N>=4){X=Jle32(P);if((X&65535)>N-4)return NULL;if((X>>16)==T){*L=X&65535;return P+4;}N-=4+(X&65535);P+=4+(X&65535);}return NULL; }
#define JHDR(P,T) ((P)->tlv_header=Jtlv((T),sizeof(*(P))-4))
#define JTRY(C) do{NTSTATUS _st=(C);if(!NT_SUCCESS(_st))return _st;}while(0)
