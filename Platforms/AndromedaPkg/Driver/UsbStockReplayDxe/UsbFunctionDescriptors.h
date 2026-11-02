// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single CDC ACM function: control interface0/interrupt IN2 + data interface1.
// This stock parser copies config+9 as its first interface; an IAD there is
// misparsed. Device class 02 and the CDC Union identify this single function.
STATIC UINT8 mCdcHs[] = {
  9,2,67,0,2,1,0,0x80,50,
  9,4,0,0,1,2,2,1,0,
  5,0x24,0,0x10,1,
  5,0x24,1,0,1,
  4,0x24,2,2,
  5,0x24,6,0,1,
  7,5,0x82,3,16,0,9,
  9,4,1,0,2,10,0,0,0,
  7,5,1,2,0,2,0,
  7,5,0x81,2,0,2,0
};
STATIC UINT8 mCdcSs[] = {
  9,2,85,0,2,1,0,0x80,50,
  9,4,0,0,1,2,2,1,0,
  5,0x24,0,0x10,1,
  5,0x24,1,0,1,
  4,0x24,2,2,
  5,0x24,6,0,1,
  7,5,0x82,3,16,0,9,6,0x30,0,0,16,0,
  9,4,1,0,2,10,0,0,0,
  7,5,1,2,0,4,0,6,0x30,0,0,0,0,
  7,5,0x81,2,0,4,0,6,0x30,0,0,0,0
};
STATIC VOID StockFunctionDescriptors (UINT32 Mode)
{
  BOOLEAN Cdc=Mode==USB_MODE_CDC, Msc=Mode==USB_MODE_STORAGE || Mode==USB_MODE_RAMDISK;
  mHsDevice.DeviceClass=mSsDevice.DeviceClass=Cdc?2:0;
  // Windows Usbser.inf matches USB\\Class_02&SubClass_02 at device level.
  // Subclass 00 enumerates without the inbox serial driver.
  mHsDevice.DeviceSubClass=mSsDevice.DeviceSubClass=Cdc?2:0;
  mHsDevice.DeviceProtocol=mSsDevice.DeviceProtocol=Cdc?1:0;
  mQualifier.DeviceClass=mHsDevice.DeviceClass;
  mQualifier.DeviceSubClass=mHsDevice.DeviceSubClass;
  mQualifier.DeviceProtocol=mHsDevice.DeviceProtocol;
  mHsDevice.IdProduct=mSsDevice.IdProduct=Cdc?0x4EE8:Mode==USB_MODE_RAMDISK?0x4EE2:Msc?0x4EE1:0xD00D;
  mHsConfig.Interface.InterfaceClass=mSsConfig.Interface.InterfaceClass=Msc?8:0xFF;
  mHsConfig.Interface.InterfaceSubClass=mSsConfig.Interface.InterfaceSubClass=Msc?6:0x42;
  mHsConfig.Interface.InterfaceProtocol=mSsConfig.Interface.InterfaceProtocol=Msc?0x50:3;
  mHsDescriptors[0]=Cdc?(VOID *)mCdcHs:(VOID *)&mHsConfig;
  mSsDescriptors[0]=Cdc?(VOID *)mCdcSs:(VOID *)&mSsConfig;
}
