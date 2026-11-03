// Existing DEBUG UFSDxe SHA256: 4c4364fefc6a46cf3b60ab724e85637bcbe5949e2091f29e33d2c2acfd19ff26
// Read path instruction/CDB fingerprints; no private function calls.
STATIC CONST struct { UINT32 Rva, Bytes, Crc; } mRpmbReadCode[] = {
  {0x4BE0, 0xA0, 0x51926566}, // counter_wrapper_tail
  {0x4D4C, 0xCC, 0x5D25E288}, // read_wrapper_tail
  {0xA93C, 0x44, 0x43A4494E}, // counter_bridge
  {0xA980, 0x50, 0x67C108A5}, // read_bridge
  {0xD808, 0x104, 0x070825E6}, // request_transport
  {0xD90C, 0x104, 0x54CC5C1B}, // response_transport
  {0xDA10, 0xDC, 0x13AF07C7}, // counter_body
  {0xDD08, 0x188, 0xDCD06517}, // read_body
  {0xE548, 0x294, 0xD7F35718}, // scsi_request
  {0xB1F0, 0x1B0, 0xFC236235}, // scsi_bridge
  {0x1BE60, 0x10, 0xBED5FE4F}, // cdb_templates
};
