// Copyright (c) 2026, ProjectMu shennong port.
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Equivalent to Usbfn RVA6E0C on valid inputs, using its original primitives.
// Hardware parameters, writes, error exits and pending insertion stay in order.
typedef UINT32 (EFIAPI *STOCK_LIST_EMPTY)(VOID *);
typedef VOID (EFIAPI *STOCK_TAKE_TD)(VOID *, VOID **);
typedef UINT32 (EFIAPI *STOCK_PREPARE_TD)(VOID *, VOID *, UINT32, UINT32, UINT32, UINT32, UINT32, UINT32);
typedef UINT32 (EFIAPI *STOCK_UPDATE_TD)(VOID *, VOID *, VOID *);
typedef UINT32 (EFIAPI *STOCK_SUBMIT_TD)(VOID *, VOID *);
typedef UINT32 (EFIAPI *STOCK_WAIT_BIT)(UINT32, UINT32, UINT32, UINT32);
typedef VOID *(EFIAPI *STOCK_INSERT_TD)(VOID *, VOID *);
typedef VOID (EFIAPI *STOCK_SETUP_TRACE)(UINT32, BOOLEAN, UINT32, VOID *);
typedef struct {
  STOCK_LIST_EMPTY Empty;
  STOCK_TAKE_TD Take;
  STOCK_PREPARE_TD Prepare;
  STOCK_UPDATE_TD Update;
  STOCK_SUBMIT_TD Submit;
  STOCK_WAIT_BIT Wait;
  STOCK_INSERT_TD Insert;
} STOCK_SETUP_OPS;
enum { StockSetupEmpty, StockSetupTake, StockSetupSg, StockSetupPrepare,
       StockSetupUpdate, StockSetupSubmit, StockSetupWait, StockSetupInsert };

STATIC UINT32 __attribute__((noinline))
StockSetupCore (UINT8 *Context, CONST STOCK_SETUP_OPS *Ops, STOCK_SETUP_TRACE Trace)
{
  VOID *Td = NULL;
  UINT8 *Sg, *Device = Context + 0x1F0;
  UINT32 Status;
  if (Trace != NULL) Trace (StockSetupEmpty, TRUE, 0, Context + 0x7B8);
  Status = Ops->Empty (Context + 0x7B8);
  if (Trace != NULL) Trace (StockSetupEmpty, FALSE, Status, Context + 0x7B8);
  if ((Status & 0xFF) == 0) return 0x8000000A;
  if (Trace != NULL) Trace (StockSetupTake, TRUE, 0, Context + 0x9B8);
  Ops->Take (Context, &Td);
  if (Trace != NULL) Trace (StockSetupTake, FALSE, 0, Td);
  if (Td == NULL) return 0x80000003;
  Sg = *(VOID **)((UINT8 *)Td + 0x450);
  if (Sg == NULL) return 0x80000003;
  if (Trace != NULL) Trace (StockSetupSg, TRUE, 0, Sg);
  *(UINT32 *)((UINT8 *)Td + 0x448) = 8;
  *(UINT64 *)Sg = 1;
  *(UINT64 *)(Sg + 0x18) = 8;
  *(VOID **)(Sg + 0x10) = *(VOID **)(Context + 0xDD0);
  if (Trace != NULL) Trace (StockSetupSg, FALSE, 0, Sg);
  if (Trace != NULL) Trace (StockSetupPrepare, TRUE, 0, Td);
  Status = Ops->Prepare (Device, (UINT8 *)Td + 0x10, 0, 0, 2, 0, 8, 0);
  if (Trace != NULL) Trace (StockSetupPrepare, FALSE, Status, Td);
  if ((INT32)Status < 0) return Status;
  if (Trace != NULL) Trace (StockSetupUpdate, TRUE, 0, Td);
  Status = Ops->Update (Device, (UINT8 *)Td + 0x10, Sg);
  if (Trace != NULL) Trace (StockSetupUpdate, FALSE, Status, Td);
  if ((INT32)Status < 0) return Status;
  if (Trace != NULL) Trace (StockSetupSubmit, TRUE, 0, Td);
  Status = Ops->Submit (Device, (UINT8 *)Td + 0x10);
  if (Trace != NULL) Trace (StockSetupSubmit, FALSE, Status, Td);
  if ((INT32)Status < 0) return Status;
  if (Trace != NULL) Trace (StockSetupWait, TRUE, 0, Td);
  Status = Ops->Wait (*(UINT32 *)(Context + 0x1E0) + 0xC80C, 0x400, 0, 0x7A120);
  if (Trace != NULL) Trace (StockSetupWait, FALSE, Status, Td);
  // The original callback queues the TD even when the final wait times out.
  if (Trace != NULL) Trace (StockSetupInsert, TRUE, 0, Td);
  Ops->Insert (Context + 0x7B8, Td);
  if (Trace != NULL) Trace (StockSetupInsert, FALSE, Status, Td);
  return Status;
}
