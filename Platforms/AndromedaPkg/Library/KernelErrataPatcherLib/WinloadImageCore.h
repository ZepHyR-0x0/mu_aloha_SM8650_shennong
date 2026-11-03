// SPDX-License-Identifier: MIT
// Validate a loaded ARM64 PE header from one already readable 4KiB page.
STATIC UINT32 Wi32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT16 Wi16(CONST UINT8 *P){return P[0]|((UINT16)P[1]<<8);}
STATIC BOOLEAN WiInspect(CONST UINT8 *Page,UINTN Bytes,UINT64 Base,UINT64 Pc,UINT32 *Size)
{
  UINT32 Pe,Image,Headers;
  if(Page==NULL || Size==NULL || Bytes<64 || Wi16(Page)!=0x5a4d)return FALSE;
  Pe=Wi32(Page+0x3c);
  if(Pe<64 || Pe>Bytes || Bytes-Pe<0x78 || Wi32(Page+Pe)!=0x4550 ||
     Wi16(Page+Pe+4)!=0xaa64 || Wi16(Page+Pe+6)==0 || Wi16(Page+Pe+6)>96 ||
     Wi16(Page+Pe+20)<0x70 || Wi16(Page+Pe+24)!=0x20b)return FALSE;
  Image=Wi32(Page+Pe+0x50);Headers=Wi32(Page+Pe+0x54);
  if(Image<8192 || Image>32*1024*1024 || (Image&4095)!=0 || Headers<Pe+0x78 || Headers>Image ||
     Base>MAX_UINT64-Image || Pc<Base || Pc-Base>=Image)return FALSE;
  *Size=Image;return TRUE;
}
