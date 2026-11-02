// SPDX-License-Identifier: BSD-2-Clause-Patent
// Scope is one Shell session. Screen/OSK are delegated; CDC supplies a second
// input and output. Only the application-level wait wrapper calls native USB.
STATIC EFI_HANDLE mConsoleHandle;
STATIC EFI_EVENT mConsoleWait;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL *mLocalIn;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *mLocalInEx;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mLocalOut;
STATIC EFI_HANDLE mLocalInHandle,mLocalOutHandle;
STATIC EFI_WAIT_FOR_EVENT mLocalWait;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mConsoleIn;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mConsoleInEx;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL mConsoleOut;
typedef struct { BOOLEAN Used;EFI_KEY_DATA Key;EFI_KEY_NOTIFY_FUNCTION Fn;VOID *Local; } CC_NOTIFY;
STATIC CC_NOTIFY mConsoleNotify[32];

STATIC VOID ConsoleCrc (EFI_TABLE_HEADER *Header)
{
  Header->CRC32=0;(VOID)gBS->CalculateCrc32(Header,Header->HeaderSize,&Header->CRC32);
}
STATIC VOID ConsolePump (VOID)
{
  if (mConsoleAttached && !mPolling && StockCurrentTpl()==TPL_APPLICATION) (VOID)StockPoll(&mApplicationService);
}
STATIC VOID ConsoleSignal (VOID)
{
  if (mConsoleWait!=NULL && mConsoleCore.KeyCount!=0) (VOID)gBS->SignalEvent(mConsoleWait);
}
STATIC BOOLEAN ConsoleMatches (CONST EFI_KEY_DATA *Expected, CONST EFI_KEY_DATA *Actual)
{
  return (Expected->Key.ScanCode==Actual->Key.ScanCode && Expected->Key.UnicodeChar==Actual->Key.UnicodeChar &&
    (Expected->KeyState.KeyShiftState==0 || Expected->KeyState.KeyShiftState==Actual->KeyState.KeyShiftState) &&
    (Expected->KeyState.KeyToggleState==0 || Expected->KeyState.KeyToggleState==Actual->KeyState.KeyToggleState));
}
STATIC VOID StockConsoleInput (CONST UINT8 *Data, UINTN Bytes)
{
  UINT32 At=mConsoleCore.KeyWrite,Count=mConsoleCore.KeyCount,J;
  UINTN I;
  if (!mConsoleAttached) return;
  CcInput(&mConsoleCore,Data,Bytes,StockTraceNowMs());
  // Dispatch key notifications once when received, including Ctrl-C while a
  // command is printing. Do not re-notify when ReadKeyStroke consumes the key.
  Count=mConsoleCore.KeyCount-Count;
  for (J=0;J<Count;J++) {
    EFI_KEY_DATA K=mConsoleCore.Keys[(At+J)%CC_KEYS];
    for (I=0;I<ARRAY_SIZE(mConsoleNotify);I++)
      if (mConsoleNotify[I].Used && ConsoleMatches(&mConsoleNotify[I].Key,&K)) (VOID)mConsoleNotify[I].Fn(&K);
  }
  ConsoleSignal();
}
STATIC EFI_STATUS StockConsoleRead (CHAR8 *Data, UINTN Capacity, UINTN *Bytes)
{
  if (Data==NULL || Bytes==NULL) return EFI_INVALID_PARAMETER;
  *Bytes=CcDrain(&mConsoleCore,(UINT8 *)Data,Capacity);return EFI_SUCCESS;
}
STATIC VOID EFIAPI ConsoleWaitNotify (EFI_EVENT Event, VOID *Context)
{
  (VOID)Context;
  // EVT_NOTIFY_WAIT executes at TPL_CALLBACK. It checks existing queues only;
  // USB HandleEvent/Send must never run from this notification.
  if (mConsoleCore.KeyCount!=0 ||
      (mLocalIn!=NULL && !EFI_ERROR(gBS->CheckEvent(mLocalIn->WaitForKey))) ||
      (mLocalInEx!=NULL && !EFI_ERROR(gBS->CheckEvent(mLocalInEx->WaitForKeyEx)))) (VOID)gBS->SignalEvent(Event);
}
STATIC EFI_STATUS EFIAPI ConsoleWait (UINTN Count, EFI_EVENT *Events, UINTN *Index)
{
  EFI_EVENT Combined[65];UINTN I,Which=0;EFI_STATUS S;
  if (!mConsoleAttached || mPoll==NULL || Count==0 || Count>64 || Events==NULL || StockCurrentTpl()!=TPL_APPLICATION)
    return mLocalWait(Count,Events,Index);
  for (I=0;I<Count;I++) Combined[I]=Events[I];
  for (;;) {
    ConsolePump();CcEscapeTimeout(&mConsoleCore,StockTraceNowMs());ConsoleSignal();
    Combined[Count]=mPoll;
    S=mLocalWait(Count+1,Combined,&Which);
    if (EFI_ERROR(S)) { if (Index!=NULL) *Index=Which;return S; }
    if (Which<Count) { if (Index!=NULL) *Index=Which;return EFI_SUCCESS; }
  }
}
STATIC EFI_STATUS EFIAPI ConsoleReset (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Extended)
{
  (VOID)This;mConsoleCore.KeyRead=mConsoleCore.KeyWrite=mConsoleCore.KeyCount=0;
  mConsoleCore.UtfLeft=mConsoleCore.Escape=0;mConsoleCore.LastCr=FALSE;
  return mLocalIn!=NULL?mLocalIn->Reset(mLocalIn,Extended):EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ConsoleResetEx (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN Extended)
{
  (VOID)This;(VOID)ConsoleReset(&mConsoleIn,Extended);
  return mLocalInEx!=NULL?mLocalInEx->Reset(mLocalInEx,Extended):EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ConsoleReadEx (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *K)
{
  (VOID)This;if (K==NULL) return EFI_INVALID_PARAMETER;
  ConsolePump();CcEscapeTimeout(&mConsoleCore,StockTraceNowMs());
  if (CcReadKey(&mConsoleCore,K)) return EFI_SUCCESS;
  if (mLocalInEx!=NULL) return mLocalInEx->ReadKeyStrokeEx(mLocalInEx,K);
  ZeroMem(K,sizeof(*K));return mLocalIn!=NULL?mLocalIn->ReadKeyStroke(mLocalIn,&K->Key):EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI ConsoleRead (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  EFI_KEY_DATA K;EFI_STATUS S;(VOID)This;if (Key==NULL) return EFI_INVALID_PARAMETER;
  S=ConsoleReadEx(&mConsoleInEx,&K);if (EFI_ERROR(S)) return S;
  *Key=K.Key;
  if ((K.KeyState.KeyShiftState&EFI_LEFT_CONTROL_PRESSED)!=0 && Key->UnicodeChar>='a' && Key->UnicodeChar<='z')
    Key->UnicodeChar=(CHAR16)(Key->UnicodeChar-'a'+1);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ConsoleState (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_TOGGLE_STATE *State)
{ (VOID)This;if (State==NULL) return EFI_INVALID_PARAMETER;return mLocalInEx!=NULL?mLocalInEx->SetState(mLocalInEx,State):EFI_UNSUPPORTED; }
STATIC EFI_STATUS EFIAPI ConsoleRegister (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *K,
                                        EFI_KEY_NOTIFY_FUNCTION Fn, VOID **Handle)
{
  UINTN I;EFI_STATUS S;(VOID)This;
  if (K==NULL || Fn==NULL || Handle==NULL) return EFI_INVALID_PARAMETER;
  for (I=0;I<ARRAY_SIZE(mConsoleNotify);I++) if (!mConsoleNotify[I].Used) {
    CC_NOTIFY *N=&mConsoleNotify[I];ZeroMem(N,sizeof(*N));N->Key=*K;N->Fn=Fn;
    if (mLocalInEx!=NULL) {
      S=mLocalInEx->RegisterKeyNotify(mLocalInEx,K,Fn,&N->Local);
      if (EFI_ERROR(S)) { ZeroMem(N,sizeof(*N));return S; }
    }
    N->Used=TRUE;*Handle=N;return EFI_SUCCESS;
  }
  return EFI_OUT_OF_RESOURCES;
}
STATIC EFI_STATUS EFIAPI ConsoleUnregister (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, VOID *Handle)
{
  UINTN I;EFI_STATUS S;(VOID)This;
  for (I=0;I<ARRAY_SIZE(mConsoleNotify);I++) if (Handle==&mConsoleNotify[I] && mConsoleNotify[I].Used) {
    if (mConsoleNotify[I].Local!=NULL && mLocalInEx!=NULL) {
      S=mLocalInEx->UnregisterKeyNotify(mLocalInEx,mConsoleNotify[I].Local);if (EFI_ERROR(S)) return S;
    }
    ZeroMem(&mConsoleNotify[I],sizeof(mConsoleNotify[I]));return EFI_SUCCESS;
  }
  return EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI ConsoleOutReset (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Extended)
{ (VOID)This;return mLocalOut->Reset(mLocalOut,Extended); }
STATIC EFI_STATUS EFIAPI ConsoleOutput (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
  EFI_STATUS S;(VOID)This;if (String==NULL) return EFI_INVALID_PARAMETER;
  S=mLocalOut->OutputString(mLocalOut,String);
  if (mConsoleAttached) { CcUnicode(&mConsoleCore,String);ConsolePump(); }
  return S;
}
STATIC EFI_STATUS EFIAPI ConsoleTestString (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{ (VOID)This;return mLocalOut->TestString(mLocalOut,String); }
STATIC EFI_STATUS EFIAPI ConsoleQuery (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode, UINTN *Columns, UINTN *Rows)
{ (VOID)This;return mLocalOut->QueryMode(mLocalOut,Mode,Columns,Rows); }
STATIC EFI_STATUS EFIAPI ConsoleMode (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode)
{ (VOID)This;return mLocalOut->SetMode(mLocalOut,Mode); }
STATIC EFI_STATUS EFIAPI ConsoleAttribute (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute)
{ (VOID)This;return mLocalOut->SetAttribute(mLocalOut,Attribute); }
STATIC EFI_STATUS EFIAPI ConsoleClear (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
  STATIC CONST UINT8 Clear[]={27,'[','2','J',27,'[','H'};(VOID)This;
  if (mConsoleAttached) CcBytes(&mConsoleCore,Clear,sizeof(Clear));
  return mLocalOut->ClearScreen(mLocalOut);
}
STATIC EFI_STATUS EFIAPI ConsoleCursor (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row)
{
  CHAR8 B[48];UINTN N;(VOID)This;
  N=AsciiSPrint(B,sizeof(B),"\x1b[%u;%uH",(UINT32)Row+1,(UINT32)Column+1);
  if (mConsoleAttached) CcBytes(&mConsoleCore,(UINT8 *)B,N);
  return mLocalOut->SetCursorPosition(mLocalOut,Column,Row);
}
STATIC EFI_STATUS EFIAPI ConsoleEnableCursor (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{ (VOID)This;return mLocalOut->EnableCursor(mLocalOut,Visible); }
STATIC VOID ConsoleRestore (VOID)
{
  UINTN I;
  if (!mConsoleAttached) return;
  // Shell restores its own logger before returning. Do not overwrite a third
  // party's console or wait hook if it has deliberately replaced ours.
  if (gST->ConIn==&mConsoleIn) { gST->ConIn=mLocalIn;gST->ConsoleInHandle=mLocalInHandle; }
  if (gST->ConOut==&mConsoleOut) { gST->ConOut=mLocalOut;gST->ConsoleOutHandle=mLocalOutHandle; }
  if (gBS->WaitForEvent==ConsoleWait) { gBS->WaitForEvent=mLocalWait;ConsoleCrc(&gBS->Hdr); }
  ConsoleCrc(&gST->Hdr);
  mFunctions.Console=FALSE;mFunctions.LogPending=FALSE;
  mConsoleAttached=FALSE;
  for (I=0;I<ARRAY_SIZE(mConsoleNotify);I++)
    if (mConsoleNotify[I].Used) (VOID)ConsoleUnregister(&mConsoleInEx,&mConsoleNotify[I]);
  StockSay("[CDCSHELL b328] detached dropped-keys=%u dropped-bytes=%u\r\n",mConsoleCore.DroppedKeys,mConsoleCore.DroppedBytes);
}
STATIC EFI_STATUS EFIAPI StockSetShellConsole (USB_BRINGUP_PROTOCOL *This, BOOLEAN Enable)
{
  EFI_STATUS S;(VOID)This;
  if (StockCurrentTpl()!=TPL_APPLICATION || mPolling) return EFI_NOT_READY;
  if (!Enable) { ConsoleRestore();return EFI_SUCCESS; }
  if (mConsoleAttached) return EFI_ALREADY_STARTED;
  if (mActiveMode!=USB_MODE_CDC || mPoll==NULL || mFunctions.Failed) return EFI_NOT_READY;
  if (gST->ConIn==NULL || gST->ConOut==NULL) return EFI_NOT_READY;
  mLocalIn=gST->ConIn;mLocalOut=gST->ConOut;mLocalInHandle=gST->ConsoleInHandle;mLocalOutHandle=gST->ConsoleOutHandle;
  mLocalInEx=NULL;
  if (mLocalInHandle!=NULL) (VOID)gBS->HandleProtocol(mLocalInHandle,&gEfiSimpleTextInputExProtocolGuid,(VOID **)&mLocalInEx);
  ZeroMem(&mConsoleCore,sizeof(mConsoleCore));
  if (mConsoleWait==NULL) {
    S=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_CALLBACK,ConsoleWaitNotify,NULL,&mConsoleWait);if (EFI_ERROR(S)) return S;
  }
  mConsoleIn=(EFI_SIMPLE_TEXT_INPUT_PROTOCOL){ConsoleReset,ConsoleRead,mConsoleWait};
  mConsoleInEx=(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL){ConsoleResetEx,ConsoleReadEx,mConsoleWait,ConsoleState,ConsoleRegister,ConsoleUnregister};
  mConsoleOut=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){ConsoleOutReset,ConsoleOutput,ConsoleTestString,ConsoleQuery,ConsoleMode,
    ConsoleAttribute,ConsoleClear,ConsoleCursor,ConsoleEnableCursor,mLocalOut->Mode};
  if (mConsoleHandle==NULL) {
    S=gBS->InstallMultipleProtocolInterfaces(&mConsoleHandle,&gEfiSimpleTextInProtocolGuid,&mConsoleIn,
      &gEfiSimpleTextInputExProtocolGuid,&mConsoleInEx,&gEfiSimpleTextOutProtocolGuid,&mConsoleOut,NULL);
    if (EFI_ERROR(S)) return S;
  }
  mLocalWait=gBS->WaitForEvent;
  gST->ConIn=&mConsoleIn;gST->ConsoleInHandle=mConsoleHandle;gST->ConOut=&mConsoleOut;gST->ConsoleOutHandle=mConsoleHandle;
  gBS->WaitForEvent=ConsoleWait;ConsoleCrc(&gBS->Hdr);ConsoleCrc(&gST->Hdr);
  mConsoleAttached=TRUE;mFunctions.Console=TRUE;mFunctions.LogPending=FALSE;
  CcUnicode(&mConsoleCore,L"\r\n[CDC Shell b328: UTF-8, Ctrl-C, exit to return]\r\n");
  StockSay("[CDCSHELL b328] attached native USB at application TPL; screen/OSK retained\r\n");
  return EFI_SUCCESS;
}
