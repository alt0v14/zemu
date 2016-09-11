#ifndef _BORDER_H_INCLUDED_
#define _BORDER_H_INCLUDED_

#include <cstdint>
#include "../../zemu.h"
#include "../device.h"
#include "sound/mixer.h"

#define MAX_SPK_VOL 0x3FFF
#define MAX_TAPE_SAVE_VOL 0x0FFF

class C_Border : public C_Device
{
public:

  static C_SndRenderer sndRenderer;
  static uint8_t portFB;

  void Init(void);
  void Close(void);

  static bool OutputByteCheckPort(uint16_t port);
  static bool OnOutputByte(uint16_t port, uint8_t value);
  static void OnFrameStart(void); // [boo]
  static void OnAfterFrameRender(void); // [boo]
};

#endif
