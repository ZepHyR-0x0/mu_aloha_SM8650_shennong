/** @file

  Synaptics TCM message layer and touch report parsing.

  Everything in this file follows the Synaptics TCM driver source; the constants
  live in TouchHw.h with their origins noted. Two properties of the protocol are
  what make a UEFI implementation practical:

    - the transport is not framed: a write is [command][payload] and a read is
      "send 0xFF, parse [0xA5][code][len lo][len hi][payload]"
    - the report layout is not fixed, but it can be SET. Writing the simplified
      descriptor that the Linux driver's touch_set_report_config() uses fixes the
      layout at 4 bytes per object, so no descriptor interpreter is needed.

**/

#include "TouchHw.h"
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>

extern EFI_STATUS
TouchSpiWrite (
  IN CONST UINT8  *TxBuffer,
  IN UINT32       Length
  );

extern EFI_STATUS
TouchSpiRead (
  OUT UINT8  *RxBuffer,
  IN  UINT32 Length
  );

extern EFI_STATUS
TouchSpiWriteRead (
  IN  CONST UINT8  *TxBuffer,
  IN  UINT32       TxLen,
  OUT UINT8        *RxBuffer,
  IN  UINT32       RxLen
  );

extern VOID
TcmSay (
  IN CONST CHAR8  *Format,
  ...
  );

/**
  Application info, as returned by CMD_GET_APPLICATION_INFO.

  Offsets follow struct syna_tcm_app_info in synaptics_tcm_core.h, which is a run
  of little-endian 16-bit fields followed by a 16-byte customer id:

    0 version   2 status   4 static_config_size   6 dynamic_config_size
    8 app_config_start_write_block   10 app_config_size
    12 max_touch_report_config_size  14 max_touch_report_payload_size
    16 customer_config_id[16]
    32 max_x   34 max_y   36 max_objects   38 num_of_buttons

  So max_x is at 32, not 30 - the 16-byte id is what pushes it there.
**/
#define APPINFO_MAX_REPORT_CONFIG  12
#define APPINFO_MAX_X        32
#define APPINFO_MAX_Y        34
#define APPINFO_MAX_OBJECTS  36
#define APPINFO_MIN_SIZE     38

//
// Filled in by TcmGetGeometry: the controller tells us how many bytes it expects
// for SET_TOUCH_REPORT_CONFIG, and the vendor driver sends exactly that many with
// the descriptor at the front and zeros after it.
//
STATIC UINT32  mReportConfigSize = 0;

//
// The controller's own report descriptor, the geometry the interpreter range checks
// against, and a one-shot flag so a decode failure is reported once instead of every
// poll. These are module state because TcmGetGeometry fills them long before the poll
// timer starts using them.
//
STATIC UINT8   mDescriptor[TCM_DESCRIPTOR_MAX];
STATIC UINT32  mDescriptorLen;
STATIC UINT32  mReportMaxX;
STATIC UINT32  mReportMaxY;
STATIC BOOLEAN mDecodeLogged;

//
// The first successful read prints where the header actually started. That one
// number settles the dummy-byte question permanently instead of leaving it to
// be argued about from driver sources for other devices.
//
STATIC BOOLEAN mMarkerSeen = FALSE;

//
// How many raw header reads have been printed. Everything the chip says starts with
// 0xA5, so a header that does not is the only evidence there is of what the bus looks
// like when the chip has nothing to say - all 0xFF means it drives the line high when
// idle, all 0x00 means it lets it fall. That distinction decides whether "no answer"
// means the chip ignored a command or never received it.
//
STATIC UINT32  mHeaderDumps;

STATIC
UINT16
TcmLe16 (
  IN CONST UINT8  *P
  )
{
  return (UINT16)(P[0] | ((UINT16)P[1] << 8));
}

/**
  Validate and unpack a frame that was clocked out in a single transaction.

  Layout: [A5][code][len lo][len hi][A5][03][payload...][5A x5]

  Used by the one-transaction path in TcmReadMessage. Keeping it separate means both
  paths apply exactly the same rules, so a frame accepted by one is a frame the other
  would accept too.

  @param[in]  Frame     Buffer holding the frame from its first byte.
  @param[in]  FrameLen  Bytes available in Frame.
  @param[out] Code      Report or status code.
  @param[out] Payload   Payload bytes, or NULL.
  @param[out] Length    Payload length.

  @retval EFI_SUCCESS       A complete, well-formed frame.
  @retval EFI_DEVICE_ERROR  Not a frame, or not a complete one.
**/
STATIC
EFI_STATUS
TcmParseFrame (
  IN  CONST UINT8  *Frame,
  IN  UINT32       FrameLen,
  OUT UINT8        *Code,
  OUT UINT8        *Payload,
  OUT UINT32       *Length
  )
{
  UINT32  PayloadLength;
  UINTN   Index;
  UINTN   Offset;

  if (FrameLen < (TCM_HEADER_SIZE + 7)) {
    return EFI_DEVICE_ERROR;
  }

  if (Frame[0] != TCM_MARKER) {
    return EFI_DEVICE_ERROR;
  }

  *Code = Frame[1];
  if ((*Code == TCM_STATUS_IDLE) || (*Code == TCM_STATUS_BUSY) ||
      (*Code == TCM_STATUS_INVALID) || (*Code == TCM_STATUS_CONTINUED))
  {
    return EFI_DEVICE_ERROR;
  }

  PayloadLength = TcmLe16 (&Frame[2]);
  if (PayloadLength == 0) {
    *Length = 0;
    return EFI_SUCCESS;
  }

  //
  // Where the payload starts depends on whether the frame arrived in one
  // transaction or two, and the controller marks the difference with the byte pair
  // that follows the header:
  //
  //   one transaction    A5 10 1C 00 | 01 01 53 33 ...      payload at +4
  //   two transactions   A5 10 1C 00   then   A5 03 | 01 01 ...   payload at +6
  //
  // b110 validated only the second shape, so it threw away the perfectly good frame
  // its own atomic read had just produced - the log shows "blk A5 10 1C 00 01 01",
  // which is the first shape, and the payload beginning immediately at +4.
  //
  Offset = TCM_HEADER_SIZE;
  if ((FrameLen > (Offset + 1)) &&
      (Frame[Offset] == TCM_MARKER) &&
      (Frame[Offset + 1] == TCM_STATUS_CONTINUED))
  {
    Offset += 2;
  }

  if ((PayloadLength > TCM_READ_LENGTH) ||
      (FrameLen < (Offset + PayloadLength + 5)))
  {
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < 5; Index++) {
    if (Frame[Offset + PayloadLength + Index] != TCM_PADDING) {
      return EFI_DEVICE_ERROR;
    }
  }

  *Length = PayloadLength;
  if (Payload != NULL) {
    CopyMem (Payload, &Frame[Offset], PayloadLength);
  }

  return EFI_SUCCESS;
}

/**
  Read one TCM message.

  Sends 0xFF and parses whatever comes back. IDLE and BUSY are treated as "try
  again" rather than errors, which is what the Linux driver does - the controller
  answers with a status code even when it has nothing to say.

  @param[out] Code     Report or status code from the header.
  @param[out] Payload  Payload bytes, or NULL.
  @param[out] Length   Payload length.

  @retval EFI_SUCCESS       A message was parsed.
  @retval EFI_DEVICE_ERROR  No valid marker within the retry budget.
**/
EFI_STATUS
TcmReadMessage (
  OUT UINT8  *Code,
  OUT UINT8  *Payload,
  OUT UINT32 *Length
  )
{
  UINT8        Block[TCM_BLOCK_READ];
  UINT8        Header[TCM_HEADER_SIZE];
  UINT8        Tail[TCM_READ_LENGTH + 7];
  EFI_STATUS   Status;
  UINTN        Attempt;
  UINTN        Index;
  UINT32       PayloadLength;

  //
  // Two reads per message, in the order the reference port on this board uses:
  // four bytes to get the header, then length + 7 more for the payload and its
  // terminator. Every read clocks out 0xFF, which is what tells the controller
  // the host is listening.
  //
  // The previous version clocked one 258-byte frame and searched it for 0xA5.
  // That tolerated a leading-offset header but could not tell a controller that
  // never spoke from one whose answer was buried in 258 bytes of idle line, and
  // it left the transaction open far past the end of the message. This shape is
  // the one that is known to work here.
  //
  for (Attempt = 0; Attempt < TCM_READ_ATTEMPTS; Attempt++) {
    //
    // Attempt 0 takes the whole frame in one transaction. This matters more than it
    // looks: the two-transaction path below reads a header and then asks for the rest,
    // and if the second read fails the first four bytes are gone - the stream is left
    // misaligned and every later attempt parses payload as a header. b109 did exactly
    // that on GET_APPLICATION_INFO: one header, one failed continuation, and then 1600
    // reads of nonsense. A single transaction cannot desynchronise anything, so it is
    // tried first and costs about 3 ms.
    //
    if (Attempt == 0) {
      ZeroMem (Block, sizeof (Block));
      if (!EFI_ERROR (TouchSpiRead (Block, sizeof (Block)))) {
        if (mHeaderDumps < 12) {
          mHeaderDumps++;
          TcmSay ("[TP] blk %02x %02x %02x %02x %02x %02x\r\n",
                  Block[0], Block[1], Block[2], Block[3], Block[4], Block[5]);
        }

        if (!EFI_ERROR (TcmParseFrame (Block, sizeof (Block), Code, Payload, Length))) {
          return EFI_SUCCESS;
        }
      }
    }

    ZeroMem (Header, sizeof (Header));
    Status = TouchSpiRead (Header, sizeof (Header));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (mHeaderDumps < 12) {
      mHeaderDumps++;
      TcmSay ("[TP] hdr %02x %02x %02x %02x\r\n",
              Header[0], Header[1], Header[2], Header[3]);
    }

    if (Header[0] != TCM_MARKER) {
      continue;
    }

    *Code = Header[1];

    if (!mMarkerSeen) {
      mMarkerSeen = TRUE;
      TcmSay ("[TP] header %02x %02x %02x %02x\r\n",
              Header[0], Header[1], Header[2], Header[3]);
    }

    //
    // IDLE (0x00), BUSY (0x02) and 0xFF are "nothing to say yet", not errors.
    //
    if ((*Code == TCM_STATUS_IDLE) || (*Code == TCM_STATUS_BUSY) || (*Code == TCM_STATUS_INVALID)) {
      MicroSecondDelay (1000);
      continue;
    }

    //
    // CONTINUED (0x03) means a fragmented message follows, which this driver does
    // not implement - the simplified report fits in one frame.
    //
    if (*Code == TCM_STATUS_CONTINUED) {
      continue;
    }

    PayloadLength = TcmLe16 (&Header[2]);
    if (PayloadLength > TCM_READ_LENGTH) {
      continue;
    }

    if (PayloadLength == 0) {
      *Length = 0;
      return EFI_SUCCESS;
    }

    //
    // Payload plus the five trailing bytes: the controller echoes the header
    // marker and a status of 3, then fills the rest with 0x5A. That tail is what
    // says the frame really ended here rather than being cut short.
    //
    ZeroMem (Tail, sizeof (Tail));
    Status = TouchSpiRead (Tail, PayloadLength + 7);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Tail[0] != TCM_MARKER) || (Tail[1] != TCM_STATUS_CONTINUED)) {
      TcmSay ("[TP] tail %02x %02x, wanted a5 03\r\n", Tail[0], Tail[1]);
      continue;
    }

    for (Index = PayloadLength + 2; Index < PayloadLength + 7; Index++) {
      if (Tail[Index] != 0x5A) {
        break;
      }
    }

    if (Index < PayloadLength + 7) {
      continue;
    }

    *Length = PayloadLength;
    if (Payload != NULL) {
      CopyMem (Payload, &Tail[2], PayloadLength);
    }

    return EFI_SUCCESS;
  }

  return EFI_DEVICE_ERROR;
}

/**
  Send a command and collect its response.

  The response does NOT always come back as STATUS_OK. CMD_IDENTIFY is answered
  with REPORT_IDENTIFY (0x10) and CMD_GET_APPLICATION_INFO with
  REPORT_APPLICATION_INFO (0x20); only commands with nothing to return - such as
  CMD_SET_TOUCH_REPORT_CONFIG - answer STATUS_OK. An earlier version of this
  function demanded STATUS_OK from every command, which would have failed
  identify with "status 0x10" on a perfectly healthy controller.

  @param[in]  Command   TCM command byte.
  @param[in]  Expected  Report or status code the answer must carry.
  @param[in]  Send      Payload to send, or NULL.
  @param[in]  SendLen   Payload length.
  @param[out] Resp      Response payload buffer.
  @param[out] RespLen   Response payload length.

  @retval EFI_SUCCESS       The command returned the expected code.
  @retval EFI_DEVICE_ERROR  Transport failure, or an unexpected code.
**/
EFI_STATUS
TcmCommand (
  IN  UINT8        Command,
  IN  UINT8        Expected,
  IN  CONST UINT8  *Send,
  IN  UINT32       SendLen,
  OUT UINT8        *Resp,
  OUT UINT32       *RespLen
  )
{
  UINT8       Tx[300];
  UINT8       Local[TCM_READ_LENGTH];
  UINT8       Code;
  EFI_STATUS  Status;
  UINTN       Attempt;
  UINT32      LocalLen;
  UINT32      Write;

  //
  // The write payload is NOT [command][payload]. syna_tcm_write_message() builds
  //   [command][length lo][length hi][payload...]
  // and counts the two length bytes as part of the payload, so even a command
  // with no data - CMD_IDENTIFY - is three bytes on the wire, not one.
  //
  if (SendLen + TCM_WRITE_OVERHEAD > sizeof (Tx)) {
    return EFI_INVALID_PARAMETER;
  }

  Tx[0] = Command;
  Tx[1] = (UINT8)(SendLen & 0xFF);
  Tx[2] = (UINT8)((SendLen >> 8) & 0xFF);
  if ((Send != NULL) && (SendLen > 0)) {
    CopyMem (&Tx[3], Send, SendLen);
  }

  //
  // The command frame is three bytes on a clock driven in software. One missed frame
  // is indistinguishable from a controller that ignored the command, so the write is
  // repeated up to three times, each with its own polling window. b109 lost both
  // GET_APPLICATION_INFO and SET_TOUCH_REPORT_CONFIG to "no answer in 100 polls" and
  // there was no way to tell which of the two it was.
  //
  for (Write = 0; Write < 3; Write++) {
    Status = TouchSpiWrite (Tx, SendLen + TCM_WRITE_OVERHEAD);
    if (EFI_ERROR (Status)) {
      TcmSay ("[TP] cmd 0x%02x: spi write failed\r\n", Command);
      return Status;
    }

    if (Write != 0) {
      TcmSay ("[TP] cmd 0x%02x: rewriting, pass %u\r\n", Command, Write + 1);
    }

  //
  // Poll for the answer the way the reference port does: ten millisecond reads,
  // because the controller answers when it is ready and not before. STATUS_OK always
  // counts as success - SET_TOUCH_REPORT_CONFIG has nothing to return - and anything
  // at or above 0x10 is a report, so it is waited out rather than treated as a
  // mismatch.
  //
  for (Attempt = 0; Attempt < TCM_CMD_POLLS; Attempt++) {
    MicroSecondDelay (10000);

    //
    // Read into a buffer this function owns, then hand the caller at most 64 bytes.
    // TcmReadMessage copies whatever length the header claims, so reading straight
    // into a caller's 64-byte Resp would let a long reply write past its end.
    //
    LocalLen = 0;
    ZeroMem (Local, sizeof (Local));
    Status = TcmReadMessage (&Code, Local, &LocalLen);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if ((Code == Expected) || (Code == TCM_STATUS_OK)) {
      if (LocalLen > 64) {
        LocalLen = 64;
      }

      *RespLen = LocalLen;
      if ((Resp != NULL) && (LocalLen > 0)) {
        CopyMem (Resp, Local, LocalLen);
      }

      return EFI_SUCCESS;
    }

    if ((Code == TCM_STATUS_IDLE) || (Code == TCM_STATUS_BUSY) ||
        (Code == TCM_STATUS_INVALID) || (Code >= 0x10))
    {
      continue;
    }

    TcmSay (
      "[TP] cmd 0x%02x -> code 0x%02x, wanted 0x%02x\r\n",
      Command,
      Code,
      Expected
      );
    return EFI_DEVICE_ERROR;
  }
  }

  TcmSay ("[TP] cmd 0x%02x: no answer in 3 x %u polls\r\n",
          Command, (UINT32)TCM_CMD_POLLS);
  return EFI_DEVICE_ERROR;
}

/**
  Ask the controller to identify itself.

  This is the smallest possible round trip, so it is the one that says whether
  the SPI path works at all: a correct answer comes back with code 0x10 and a
  payload starting with "A" (0x41) for an application-mode controller.
**/
EFI_STATUS
TcmIdentify (
  OUT UINT8  *Payload,
  OUT UINT32 *Length
  )
{
  return TcmCommand (TCM_CMD_IDENTIFY, TCM_REPORT_IDENTIFY, NULL, 0, Payload, Length);
}

/**
  Fetch application info and pull out the geometry this driver needs.
**/
EFI_STATUS
TcmGetGeometry (
  OUT UINT32  *MaxX,
  OUT UINT32  *MaxY,
  OUT UINT32  *MaxObjects
  )
{
  //
  // Sized to the read limit, not to the 56 bytes an app info actually is: the
  // length comes out of the message header, and a buffer smaller than that header
  // allows is a buffer overflow waiting for one odd reply.
  //
  UINT8       Info[TCM_READ_LENGTH];
  UINT32      Length;
  EFI_STATUS  Status;

  ZeroMem (Info, sizeof (Info));
  Length = 0;

  Status = TcmCommand (
             TCM_CMD_GET_APPLICATION_INFO,
             TCM_REPORT_APPLICATION_INFO,
             NULL,
             0,
             Info,
             &Length
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Length < APPINFO_MIN_SIZE) {
    TcmSay ("[TP] app info short (%u bytes)\r\n", Length);
    return EFI_DEVICE_ERROR;
  }

  *MaxX       = TcmLe16 (&Info[APPINFO_MAX_X]);
  *MaxY       = TcmLe16 (&Info[APPINFO_MAX_Y]);
  *MaxObjects = TcmLe16 (&Info[APPINFO_MAX_OBJECTS]);

  mReportConfigSize = TcmLe16 (&Info[APPINFO_MAX_REPORT_CONFIG]);
  mReportMaxX       = *MaxX;
  mReportMaxY       = *MaxY;

  TcmSay (
    "[TP] geometry %u x %u, %u objects, report cfg size %u\r\n",
    *MaxX,
    *MaxY,
    *MaxObjects,
    mReportConfigSize
    );
  return EFI_SUCCESS;
}

/**
  Install the simplified report descriptor.

  The same sequence the Linux driver's touch_set_report_config() sends. After
  this the report layout is known by construction:

    4 bits index, 4 bits classification, 12 bits X, 12 bits Y, per object

  which is 4 bytes per object and fits 3200 rows in 12 bits.
**/
EFI_STATUS
TcmSetSimpleReportConfig (
  VOID
  )
{
  STATIC CONST UINT8  Descriptor[11] = {
    TOUCH_FOREACH_ACTIVE_OBJECT,
    TOUCH_OBJECT_N_INDEX,          4,
    TOUCH_OBJECT_N_CLASSIFICATION, 4,
    TOUCH_OBJECT_N_X_POSITION,     12,
    TOUCH_OBJECT_N_Y_POSITION,     12,
    TOUCH_FOREACH_END,
    TOUCH_END
  };
  UINT8       Payload[128];
  UINT8       Resp[64];
  UINT32      Length;
  UINT32      RespLen;
  EFI_STATUS  Status;

  //
  // The vendor driver allocates max_touch_report_config_size bytes, fills the
  // front with the descriptor and sends the whole buffer. Sending only the 11
  // meaningful bytes would be a shorter payload than the controller asked for,
  // so pad to the size it reported. Fall back to the descriptor length when the
  // app info was never read.
  //
  Length = mReportConfigSize;
  if ((Length < sizeof (Descriptor)) || (Length > sizeof (Payload))) {
    Length = sizeof (Descriptor);
  }

  ZeroMem (Payload, sizeof (Payload));
  CopyMem (Payload, Descriptor, sizeof (Descriptor));

  ZeroMem (Resp, sizeof (Resp));
  RespLen = 0;

  Status = TcmCommand (
             TCM_CMD_SET_TOUCH_REPORT_CONFIG,
             TCM_STATUS_OK,
             Payload,
             Length,
             Resp,
             &RespLen
             );
  TcmSay ("[TP] set report config (%u bytes) -> %r\r\n", Length, Status);
  return Status;
}

//
// The controller's own report descriptor, and what the interpreter needs from it.
//
// Read with CMD_GET_TOUCH_REPORT_CONFIG (0x25), which is the route the reference port
// on this board takes: it never writes a descriptor of its own, it asks the controller
// for the one it was built with and decodes reports against that. The kernel driver
// does the opposite - writes a simplified layout with 0x26 - but a descriptor written
// by the host is only correct if the controller accepts it, and this driver has no
// evidence that it does.
//

//
// The interpreter below is a port of hd_layout_parse / hd_bits / hd_field / hd_object
// / hd_decode from UniXNU-houji-main native/touch/HoujiTouchDescriptor.h, which is the
// version that decodes real contacts on this hardware. It is bounds checked on every
// field: a descriptor that does not fit, or data shorter than the descriptor claims,
// is rejected rather than read past.
//
typedef struct {
  UINT32  Begin;
  UINT32  End;
  UINT32  Finish;
  UINT32  Active;
} TCM_LAYOUT;

STATIC
EFI_STATUS
TcmLayoutParse (
  IN  CONST UINT8     *Cfg,
  IN  UINT32          Bytes,
  OUT TCM_LAYOUT      *Layout
  )
{
  UINT32  Index;
  UINT32  In;
  UINT32  Loops;
  UINT32  Fields;
  UINT32  Progress;
  UINT32  Token;
  UINT32  Bits;

  if ((Cfg == NULL) || (Layout == NULL) || (Bytes == 0) || (Bytes > TCM_DESCRIPTOR_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Layout, sizeof (*Layout));
  In = 0;
  Loops = 0;
  Fields = 0;
  Progress = 0;

  for (Index = 0; Index < Bytes; ) {
    Token = Cfg[Index++];

    if (Token == 0) {
      //
      // END. The descriptor is only well formed if the loop closed exactly once, all
      // four per-object fields were declared, and the loop body consumed some bits.
      //
      if ((In != 0) || (Loops != 1) || ((Fields & 14) != 14) || (Progress == 0)) {
        return EFI_DEVICE_ERROR;
      }

      Layout->Finish = Index;
      return EFI_SUCCESS;
    }

    if ((Token == TCM_DESC_FOREACH_ACTIVE) || (Token == TCM_DESC_FOREACH_OBJECT)) {
      if ((In != 0) || (Loops != 0)) {
        return EFI_DEVICE_ERROR;
      }

      In            = 1;
      Loops         = 1;
      Layout->Active = (Token == TCM_DESC_FOREACH_ACTIVE) ? 1 : 0;
      Layout->Begin = Index;
      continue;
    }

    if (Token == TCM_DESC_FOREACH_END) {
      if (In == 0) {
        return EFI_DEVICE_ERROR;
      }

      In        = 0;
      Layout->End = Index - 1;
      continue;
    }

    if (Token == TCM_DESC_PAD) {
      continue;
    }

    if (Index >= Bytes) {
      return EFI_DEVICE_ERROR;
    }

    Bits = Cfg[Index++];
    if (Bits == 0) {
      return EFI_DEVICE_ERROR;
    }

    if ((Token >= TCM_DESC_INDEX) && (Token <= TCM_DESC_Y)) {
      if ((In == 0) || (Bits > (((Token >= TCM_DESC_X) ? 16U : 8U)))) {
        return EFI_DEVICE_ERROR;
      }

      Fields |= 1U << (Token - TCM_DESC_INDEX);
    }

    if (Token == TCM_DESC_NUM_ACTIVE) {
      if ((In != 0) || (Bits > 8)) {
        return EFI_DEVICE_ERROR;
      }
    }

    if (In != 0) {
      Progress += Bits;
    }
  }

  return EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
TcmBits (
  IN  CONST UINT8  *Data,
  IN  UINT32       Bytes,
  IN  OUT UINT32   *Offset,
  IN  UINT32       Bits,
  OUT UINT32       *Value
  )
{
  UINT32  Index;
  UINT32  Result;

  if ((Data == NULL) || (Offset == NULL) || (Value == NULL) ||
      (Bits == 0) || (Bits > 32) || (*Offset > Bytes * 8) ||
      (Bits > Bytes * 8 - *Offset))
  {
    return EFI_DEVICE_ERROR;
  }

  Result = 0;
  for (Index = 0; Index < Bits; Index++) {
    Result |= (UINT32)((Data[(*Offset + Index) / 8] >> ((*Offset + Index) % 8)) & 1U) << Index;
  }

  *Offset += Bits;
  *Value   = Result;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
TcmField (
  IN  CONST UINT8  *Cfg,
  IN  UINT32       Limit,
  IN  OUT UINT32   *Index,
  IN  CONST UINT8  *Data,
  IN  UINT32       Bytes,
  IN  OUT UINT32   *Offset,
  OUT UINT32       *Value
  )
{
  UINT32  Bits;

  if ((Cfg == NULL) || (Index == NULL) || (*Index >= Limit)) {
    return EFI_DEVICE_ERROR;
  }

  Bits = Cfg[(*Index)++];
  if ((Bits == 0) || (*Offset > Bytes * 8) || (Bits > Bytes * 8 - *Offset)) {
    return EFI_DEVICE_ERROR;
  }

  if (Bits <= 32) {
    return TcmBits (Data, Bytes, Offset, Bits, Value);
  }

  *Offset += Bits;
  *Value   = 0;
  return EFI_SUCCESS;
}

/**
  Decode one object and say whether it is a finger.

  The classification check is the part worth taking from the other implementation in
  usb-touch-isolated: it refuses anything that is not a finger (1) or a glove (2),
  where this driver used to accept any object whose coordinates were not all ones. A
  hover or a stylus object would then have been reported as a touch.

  @param[out] IsFinger  TRUE when the object is a finger inside the panel.
**/
STATIC
EFI_STATUS
TcmObjectDecode (
  IN  CONST UINT8     *Cfg,
  IN  CONST TCM_LAYOUT *Layout,
  IN  CONST UINT8     *Data,
  IN  UINT32          Bytes,
  IN  OUT UINT32      *Offset,
  IN  UINT32          Ordinal,
  IN  OUT UINT32      *Seen,
  OUT UINT32          *FingerX,
  OUT UINT32          *FingerY,
  OUT BOOLEAN         *IsFinger
  )
{
  UINT32      Index;
  UINT32      Token;
  UINT32      Value;
  UINT32      Id;
  UINT32      Kind;
  UINT32      X;
  UINT32      Y;
  UINT32      Flags;
  EFI_STATUS  Status;

  Id    = Ordinal;
  Kind  = 0;
  X     = 0;
  Y     = 0;
  Flags = 0;

  for (Index = Layout->Begin; Index < Layout->End; ) {
    Token = Cfg[Index++];
    Value = 0;

    if (Token == TCM_DESC_PAD) {
      *Offset = (*Offset + 7) & ~7U;
      if (*Offset > Bytes * 8) {
        return EFI_DEVICE_ERROR;
      }

      continue;
    }

    if (Token < 5) {
      return EFI_DEVICE_ERROR;
    }

    Status = TcmField (Cfg, Layout->End, &Index, Data, Bytes, Offset, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Token == TCM_DESC_INDEX) {
      Id = Value;
    }

    if (Token == TCM_DESC_CLASS) {
      Kind   = Value;
      Flags |= 1U;
    }

    if (Token == TCM_DESC_X) {
      X      = Value;
      Flags |= 2U;
    }

    if (Token == TCM_DESC_Y) {
      Y      = Value;
      Flags |= 4U;
    }
  }

  *IsFinger = FALSE;

  if ((Id >= TCM_MAX_CONTACTS) || ((*Seen & (1U << Id)) != 0) ||
      (Kind > 9) || (Kind == 7) || ((Flags & 1U) == 0))
  {
    return EFI_DEVICE_ERROR;
  }

  *Seen |= 1U << Id;

  if ((Kind != TCM_OBJECT_FINGER) && (Kind != TCM_OBJECT_GLOVE)) {
    return EFI_SUCCESS;
  }

  if ((Flags != 7U) || (X > mReportMaxX) || (Y > mReportMaxY)) {
    return EFI_DEVICE_ERROR;
  }

  *FingerX  = X;
  *FingerY  = Y;
  *IsFinger = TRUE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
TcmDecodeReport (
  IN  CONST UINT8  *Data,
  IN  UINT32       Bytes,
  OUT UINT32       *X,
  OUT UINT32       *Y,
  OUT UINT32       *Contacts
  )
{
  TCM_LAYOUT  Layout;
  UINT32      Offset;
  UINT32      Active;
  UINT32      HaveCount;
  UINT32      Seen;
  UINT32      Index;
  UINT32      Token;
  UINT32      Value;
  UINT32      Object;
  UINT32      Count;
  UINT32      Before;
  UINT32      FingerX;
  UINT32      FingerY;
  BOOLEAN     IsFinger;
  EFI_STATUS  Status;

  *Contacts = 0;
  *X        = 0;
  *Y        = 0;

  Status = TcmLayoutParse (mDescriptor, mDescriptorLen, &Layout);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Offset    = 0;
  Active    = 0;
  HaveCount = 0;
  Seen      = 0;
  FingerX   = 0;
  FingerY   = 0;

  for (Index = 0; Index < Layout.Finish; ) {
    Token = mDescriptor[Index++];
    Value = 0;

    if (Token == 0) {
      break;
    }

    if ((Token == TCM_DESC_FOREACH_ACTIVE) || (Token == TCM_DESC_FOREACH_OBJECT)) {
      Count = (Layout.Active != 0)
              ? (HaveCount ? Active : TCM_MAX_CONTACTS)
              : TCM_MAX_CONTACTS;
      if (Count > TCM_MAX_CONTACTS) {
        return EFI_DEVICE_ERROR;
      }

      for (Object = 0; Object < Count; Object++) {
        if ((Layout.Active != 0) && (HaveCount == 0) && (Offset == Bytes * 8)) {
          break;
        }

        Before = Offset;
        Status = TcmObjectDecode (
                   mDescriptor,
                   &Layout,
                   Data,
                   Bytes,
                   &Offset,
                   Object,
                   &Seen,
                   &FingerX,
                   &FingerY,
                   &IsFinger
                   );
        if (EFI_ERROR (Status)) {
          return Status;
        }

        if (IsFinger) {
          if (*Contacts == 0) {
            *X = FingerX;
            *Y = FingerY;
          }

          (*Contacts)++;
        }

        if (Offset <= Before) {
          return EFI_DEVICE_ERROR;
        }
      }

      Index = Layout.End + 1;
      continue;
    }

    if (Token == TCM_DESC_FOREACH_END) {
      return EFI_DEVICE_ERROR;
    }

    if (Token == TCM_DESC_PAD) {
      Offset = (Offset + 7) & ~7U;
      if (Offset > Bytes * 8) {
        return EFI_DEVICE_ERROR;
      }

      continue;
    }

    Status = TcmField (mDescriptor, Layout.Finish, &Index, Data, Bytes, &Offset, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Token == TCM_DESC_NUM_ACTIVE) {
      if (Value > TCM_MAX_CONTACTS) {
        return EFI_DEVICE_ERROR;
      }

      Active    = Value;
      HaveCount = 1;
    }
  }

  return EFI_SUCCESS;
}

/**
  Ask the controller for its report descriptor and keep it.

  @param[out] Length  Descriptor length, or NULL.

  @retval EFI_SUCCESS  A descriptor was read and stored.
**/
EFI_STATUS
TcmLoadDescriptor (
  OUT UINT32  *Length
  )
{
  UINT8       Buffer[TCM_READ_LENGTH];
  UINT32      Len;
  EFI_STATUS  Status;

  Len = 0;
  ZeroMem (Buffer, sizeof (Buffer));
  Status = TcmCommand (
             TCM_CMD_GET_TOUCH_REPORT_CONFIG,
             TCM_REPORT_TOUCH_REPORT_CONFIG,
             NULL,
             0,
             Buffer,
             &Len
             );
  if (EFI_ERROR (Status) || (Len == 0) || (Len > TCM_DESCRIPTOR_MAX)) {
    TcmSay ("[TP] descriptor unusable: %r, len=%u\r\n", Status, Len);
    mDescriptorLen = 0;
    return EFI_DEVICE_ERROR;
  }

  ZeroMem (mDescriptor, sizeof (mDescriptor));
  CopyMem (mDescriptor, Buffer, Len);
  mDescriptorLen = Len;

  //
  // Print the first 40 bytes. This is the only place the layout of a touch report can
  // be read off the hardware rather than assumed, so it belongs in the log.
  //
  {
    CHAR8  Line[200];
    UINTN  LineLen;
    UINTN  Offset;
    UINTN  Limit;

    LineLen = AsciiSPrint (Line, sizeof (Line), "[TP] descriptor (%u):", Len);
    Limit   = (Len < 40) ? Len : 40;
    for (Offset = 0; Offset < Limit; Offset++) {
      LineLen += AsciiSPrint (&Line[LineLen], sizeof (Line) - LineLen, " %02x", mDescriptor[Offset]);
    }

    AsciiSPrint (&Line[LineLen], sizeof (Line) - LineLen, "\r\n");
    SerialPortWrite ((UINT8 *)Line, AsciiStrLen (Line));
  }

  if (Length != NULL) {
    *Length = Len;
  }

  return EFI_SUCCESS;
}

/**
  Read one touch report and decode the first active object.

  With the descriptor above each object is exactly four bytes:
    byte 0 low nibble  = index, high nibble = classification
    byte 1             = X bits 3..0 in the high nibble, bits 11..4 in bits 3..0
    ...
  which is easier to read as a single 32-bit little-endian word: index in bits
  0..3, classification in 4..7, X in 8..19, Y in 20..31.

  @param[out] X        Screen X, or 0 when nothing is touching.
  @param[out] Y        Screen Y.
  @param[out] Touching TRUE when at least one object decoded.
  @param[out] Raw      Raw payload, for logging when the layout looks wrong.
  @param[out] RawLen   Raw payload length.

  @retval EFI_SUCCESS       A report was read (Touching says whether it had data).
  @retval EFI_DEVICE_ERROR  No touch report arrived this poll.
**/
EFI_STATUS
TcmPollTouch (
  OUT UINT32  *X,
  OUT UINT32  *Y,
  OUT BOOLEAN *Touching,
  OUT UINT8   *Raw,
  OUT UINT32  *RawLen
  )
{
  UINT8       Payload[TCM_READ_LENGTH];
  UINT8       Code;
  UINT32      Length;
  EFI_STATUS  Status;
  UINT32      Contacts;

  *Touching = FALSE;
  *X        = 0;
  *Y        = 0;
  Length    = 0;

  Status = TcmReadMessage (&Code, Payload, &Length);
  if (EFI_ERROR (Status)) {
    //
    // Not a failure, and this distinction is the whole point.
    //
    // A controller with nothing to report answers A5 00 00 00 - code 0, length 0,
    // IDLE - and TcmReadMessage has no way to say "no frame" other than returning an
    // error, so it returns one. The poll timer used to count that as a dead link and
    // cancelled itself after eight polls, which is 160 ms: the absolute pointer was
    // installed, went deaf almost immediately, and no tap could ever reach it.
    //
    // Idle means "nothing is touching the screen", which is the normal state.
    //
    *Touching = FALSE;
    *X        = 0;
    *Y        = 0;
    *RawLen   = 0;
    return EFI_SUCCESS;
  }

  if (Code != TCM_REPORT_TOUCH) {
    //
    // Anything else is not an error: the controller interleaves status and
    // printf reports with touch data.
    //
    *RawLen = Length;
    if (Length > 0) {
      CopyMem (Raw, Payload, MIN (Length, TCM_READ_LENGTH));
    }
    return EFI_SUCCESS;
  }

  *RawLen = Length;
  if (Length > 0) {
    CopyMem (Raw, Payload, MIN (Length, TCM_READ_LENGTH));
  }

  //
  // Decode against the controller's own descriptor. The fixed four-bytes-per-object
  // loop that used to be here assumed the simplified layout this driver never managed
  // to install, so it read the native report as if it were a layout of its own
  // invention - which is why an absolute pointer could be installed and still never
  // report a contact.
  //
  Contacts = 0;
  Status   = TcmDecodeReport (Payload, Length, X, Y, &Contacts);
  if (EFI_ERROR (Status)) {
    if (!mDecodeLogged) {
      mDecodeLogged = TRUE;
      TcmSay ("[TP] decode failed: %r, len=%u, descriptor=%u bytes\r\n",
              Status, Length, mDescriptorLen);
    }

    return EFI_SUCCESS;
  }

  if (Contacts > 0) {
    *Touching = TRUE;
  }

  return EFI_SUCCESS;
}
