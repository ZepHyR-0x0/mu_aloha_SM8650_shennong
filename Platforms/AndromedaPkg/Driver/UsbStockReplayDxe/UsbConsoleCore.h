// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded UTF-8 / VT input and UTF-8 output queues. No hardware or allocation.
#define CC_KEYS 2048U
#define CC_OUTPUT 65536U
typedef struct {
  EFI_KEY_DATA Keys[CC_KEYS];
  UINT32 KeyRead,KeyWrite,KeyCount;
  UINT8 Output[CC_OUTPUT];
  UINT32 OutRead,OutWrite,OutCount;
  UINT32 DroppedKeys,DroppedBytes;
  UINT32 UtfCode,UtfMin;
  UINT8 UtfLeft,Escape;
  UINT16 EscapeNumber;
  UINT64 EscapeTime;
  BOOLEAN LastCr;
} CDC_CONSOLE_CORE;
STATIC CDC_CONSOLE_CORE mConsoleCore;
STATIC VOID CcKey (CDC_CONSOLE_CORE *C, UINT16 Scan, CHAR16 Char, UINT32 Shift)
{
  EFI_KEY_DATA *K;
  if (C->KeyCount==CC_KEYS) { C->DroppedKeys++;return; }
  K=&C->Keys[C->KeyWrite];ZeroMem(K,sizeof(*K));K->Key.ScanCode=Scan;K->Key.UnicodeChar=Char;
  K->KeyState.KeyShiftState=EFI_SHIFT_STATE_VALID|Shift;
  C->KeyWrite=(C->KeyWrite+1)%CC_KEYS;C->KeyCount++;
}
STATIC VOID CcEscapeTimeout (CDC_CONSOLE_CORE *C, UINT64 Ms)
{
  if (C->Escape!=0 && Ms-C->EscapeTime>=100) {
    CcKey(C,SCAN_ESC,0,0);C->Escape=0;C->EscapeNumber=0;
  }
}
STATIC VOID CcInput (CDC_CONSOLE_CORE *C, CONST UINT8 *Data, UINTN Bytes, UINT64 Ms)
{
  UINTN I;UINT8 B;UINT16 Scan;
  CcEscapeTimeout(C,Ms);
  for (I=0;I<Bytes;I++) {
    B=Data[I];
    if (C->Escape!=0) {
      C->EscapeTime=Ms;
      if (C->Escape==1 && (B=='[' || B=='O')) { C->Escape=2;continue; }
      if (C->Escape==2) {
        if (B>='0' && B<='9') { C->EscapeNumber=MIN(999,C->EscapeNumber*10+B-'0');continue; }
        Scan=B=='A'?SCAN_UP:B=='B'?SCAN_DOWN:B=='C'?SCAN_RIGHT:B=='D'?SCAN_LEFT:
             B=='H'?SCAN_HOME:B=='F'?SCAN_END:0;
        if (B=='~') Scan=C->EscapeNumber==3?SCAN_DELETE:C->EscapeNumber==5?SCAN_PAGE_UP:
                            C->EscapeNumber==6?SCAN_PAGE_DOWN:C->EscapeNumber==1?SCAN_HOME:
                            C->EscapeNumber==4?SCAN_END:0;
        C->Escape=0;C->EscapeNumber=0;
        if (Scan!=0) CcKey(C,Scan,0,0);
        // Unsupported escape sequences are discarded, never interpreted as commands.
        continue;
      }
      CcKey(C,SCAN_ESC,0,0);C->Escape=0;
    }
    if (C->UtfLeft!=0) {
      if ((B&0xC0)==0x80) {
        C->UtfCode=(C->UtfCode<<6)|(B&63);
        if (--C->UtfLeft==0) {
          UINT32 U=C->UtfCode;
          if (U<C->UtfMin || U>0x10FFFF || (U>=0xD800 && U<=0xDFFF)) CcKey(C,0,0xFFFD,0);
          else if (U<=0xFFFF) CcKey(C,0,(CHAR16)U,0);
          else { U-=0x10000;CcKey(C,0,(CHAR16)(0xD800+(U>>10)),0);CcKey(C,0,(CHAR16)(0xDC00+(U&1023)),0); }
        }
        continue;
      }
      C->UtfLeft=0;CcKey(C,0,0xFFFD,0);
    }
    if (B==27) { C->Escape=1;C->EscapeTime=Ms;continue; }
    if (B>=0xC2 && B<=0xDF) { C->UtfLeft=1;C->UtfCode=B&31;C->UtfMin=0x80;continue; }
    if (B>=0xE0 && B<=0xEF) { C->UtfLeft=2;C->UtfCode=B&15;C->UtfMin=0x800;continue; }
    if (B>=0xF0 && B<=0xF4) { C->UtfLeft=3;C->UtfCode=B&7;C->UtfMin=0x10000;continue; }
    if (B>=128) { CcKey(C,0,0xFFFD,0);continue; }
    if (B=='\n' && C->LastCr) { C->LastCr=FALSE;continue; }
    C->LastCr=B=='\r';
    if (B=='\n') B='\r';
    if (B==127) B=8;
    if (B>0 && B<=26 && B!=8 && B!=9 && B!=13) CcKey(C,0,(CHAR16)('a'+B-1),EFI_LEFT_CONTROL_PRESSED);
    else if (B!=0) CcKey(C,0,(CHAR16)B,0);
  }
}
STATIC BOOLEAN CcReadKey (CDC_CONSOLE_CORE *C, EFI_KEY_DATA *K)
{
  if (C->KeyCount==0) return FALSE;
  *K=C->Keys[C->KeyRead];C->KeyRead=(C->KeyRead+1)%CC_KEYS;C->KeyCount--;return TRUE;
}
STATIC VOID CcBytes (CDC_CONSOLE_CORE *C, CONST UINT8 *Bytes, UINTN N)
{
  UINTN I;
  // Keep UTF-8 code points / VT sequences atomic on overflow.
  if (N>CC_OUTPUT-C->OutCount) { C->DroppedBytes+=(UINT32)N;return; }
  for (I=0;I<N;I++) { C->Output[C->OutWrite]=Bytes[I];C->OutWrite=(C->OutWrite+1)%CC_OUTPUT; }
  C->OutCount+=(UINT32)N;
}
STATIC VOID CcUnicode (CDC_CONSOLE_CORE *C, CONST CHAR16 *S)
{
  UINT32 U;UINT8 B[4];UINTN N;
  while (*S!=0) {
    U=*S++;
    if (U>=0xD800 && U<=0xDBFF && *S>=0xDC00 && *S<=0xDFFF) U=0x10000+((U-0xD800)<<10)+(*S++-0xDC00);
    else if (U>=0xD800 && U<=0xDFFF) U=0xFFFD;
    if (U<0x80) { B[0]=(UINT8)U;N=1; }
    else if (U<0x800) { B[0]=(UINT8)(0xC0|(U>>6));B[1]=(UINT8)(0x80|(U&63));N=2; }
    else if (U<0x10000) { B[0]=(UINT8)(0xE0|(U>>12));B[1]=(UINT8)(0x80|((U>>6)&63));B[2]=(UINT8)(0x80|(U&63));N=3; }
    else { B[0]=(UINT8)(0xF0|(U>>18));B[1]=(UINT8)(0x80|((U>>12)&63));B[2]=(UINT8)(0x80|((U>>6)&63));B[3]=(UINT8)(0x80|(U&63));N=4; }
    CcBytes(C,B,N);
  }
}
STATIC UINTN CcDrain (CDC_CONSOLE_CORE *C, UINT8 *Data, UINTN Capacity)
{
  UINTN I,N=MIN(C->OutCount,Capacity);
  for (I=0;I<N;I++) { Data[I]=C->Output[C->OutRead];C->OutRead=(C->OutRead+1)%CC_OUTPUT; }
  C->OutCount-=(UINT32)N;return N;
}
