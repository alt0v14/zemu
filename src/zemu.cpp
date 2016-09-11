#include <string.h>
#include <ctype.h>
#include "zemu_env.h"
#include "zemu.h"
#include "exceptions.h"
#include "bin.h"
#include "lib_wd1793/wd1793_chip.h"
#include "dirwork.h"
#include "font.h"
#include "dialog.h"
#include "graphics.h"
#include "cpu_trace.h"
#include "tape/tape.h"
#include "labels.h"
#include "joystick_manager.h"		// [boo_boo]

#include "renderer/render_speccy.h"
#include "renderer/render_16c.h"
#include "renderer/render_multicolor.h"

#include "devs.h"

#include "snap_z80.h"
#include "snap_sna.h"

#include "ftbos_font.h"
#include "font_64.h"

#include "file.h"

#include <cctype>
#include <algorithm>

#if defined(__APPLE__)
  #include <SDL_Thread.h>
  #include <SDL_Mutex.h>
#endif

#define SNAP_FORMAT_Z80 0
#define SNAP_FORMAT_SNA 1

unsigned turboMultiplier = 1;
unsigned turboMultiplierNx = 1;
bool unturbo = false;
bool unturboNx = false;

bool isPaused = false;
bool isPausedNx = false;

bool joyOnKeyb = false;

Z80EX_CONTEXT *cpu;
uint64_t cpuClk, devClk, lastDevClk, devClkCounter;
s_Params params;
SDL_Surface *screen, *realScreen, *scrSurf[2];
int PITCH, REAL_PITCH;
bool drawFrame;
int frames;
C_Font font, fixed_font;
bool disableSound = false;
bool doCopyOfSurfaces = false;

int videoSpec;
int actualWidth;
int actualHeight;

char tempFolderName[MAX_PATH];

bool recordWav = false;
const char *wavFileName = "output.wav"; // TODO: make configurable + full filepath

// bool isLongImageWrited = false;
// const char * longImageFileName = "longimage.ppm";
// C_File longImageFile;
// long longImageWidth;
// long longImageHeight;
// int longImagePos;

bool isAvgImageWrited = false;
const char *avgImageFileName = "avgimage.ppm";
long *avgImageBuffer;
// int avgImageFrames;

#if defined(__APPLE__)
SDL_Thread *upadteScreenThread = nullptr;
SDL_sem *updateScreenThreadSem;
volatile bool updateScreenThreadActive = true;

int UpdateScreenThreadFunc(void *param);
#endif

//--------------------------------------------------------------------------------------------------

ptrOnReadByteFunc *devMapRead;
bool (** devMapWrite)(uint16_t, uint8_t);
bool (** devMapInput)(uint16_t, uint8_t &);
bool (** devMapOutput)(uint16_t, uint8_t);

ptrOnReadByteFunc devMapRead_base[0x20000];
bool (* devMapWrite_base[0x10000])(uint16_t, uint8_t);
bool (* devMapInput_base[0x10000])(uint16_t, uint8_t &);
bool (* devMapOutput_base[0x10000])(uint16_t, uint8_t);

ptrOnReadByteFunc devMapRead_trdos[0x20000];
bool (* devMapInput_trdos[0x10000])(uint16_t, uint8_t &);
bool (* devMapOutput_trdos[0x10000])(uint16_t, uint8_t);

//--------------------------------------------------------------------------------------------------

struct s_ReadItem
{
  ptrOnReadByteFunc(* check)(uint16_t, bool);
//	bool (* func)(uint16_t, bool, uint8_t&);
};

struct s_WriteItem
{
  bool (* check)(uint16_t);
  bool (* func)(uint16_t, uint8_t);
};

struct s_InputItem
{
  bool (* check)(uint16_t);
  bool (* func)(uint16_t, uint8_t &);
};

typedef s_WriteItem s_OutputItem;

struct s_SdlItem
{
  int eventType;
  bool (* func)(SDL_Event &);
};

//--------------------------------------------------------------------------------------------------

s_ReadItem hnd_z80read[MAX_HANDLERS];
s_WriteItem hnd_z80write[MAX_HANDLERS];
s_InputItem hnd_z80input[MAX_HANDLERS];
s_OutputItem hnd_z80output[MAX_HANDLERS];
void (* hnd_frameStart[MAX_HANDLERS])(void);
void (* hnd_afterFrameRender[MAX_HANDLERS])(void);
s_SdlItem hnd_sdl[MAX_HANDLERS];
void (* hnd_reset[MAX_HANDLERS])(void);

int cnt_z80read = 0;
int cnt_z80write = 0;
int cnt_z80input = 0;
int cnt_z80output = 0;
int cnt_frameStart = 0;
int cnt_afterFrameRender = 0;
int cnt_sdl = 0;
int cnt_reset = 0;

void AttachZ80ReadHandler(ptrOnReadByteFunc(* check)(uint16_t, bool))
{
  if (cnt_z80read >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");

  s_ReadItem item;
  item.check = check;

  hnd_z80read[cnt_z80read++] = item;
}

void AttachZ80WriteHandler(bool (* check)(uint16_t), bool (* func)(uint16_t, uint8_t))
{
  if (cnt_z80write >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");

  s_WriteItem item;
  item.check = check;
  item.func = func;

  hnd_z80write[cnt_z80write++] = item;
}

void AttachZ80InputHandler(bool (* check)(uint16_t), bool (* func)(uint16_t, uint8_t &))
{
  if (cnt_z80input >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");

  s_InputItem item;
  item.check = check;
  item.func = func;

  hnd_z80input[cnt_z80input++] = item;
}

void AttachZ80OutputHandler(bool (* check)(uint16_t), bool (* func)(uint16_t, uint8_t))
{
  if (cnt_z80output >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");

  s_OutputItem item;
  item.check = check;
  item.func = func;

  hnd_z80output[cnt_z80output++] = item;
}

void AttachFrameStartHandler(void (* func)(void))
{
  if (cnt_frameStart >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");
  hnd_frameStart[cnt_frameStart++] = func;
}

void AttachAfterFrameRenderHandler(void (* func)(void))
{
  if (cnt_afterFrameRender >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");
  hnd_afterFrameRender[cnt_afterFrameRender++] = func;
}

void AttachSDLHandler(int eventType, bool (* func)(SDL_Event &))
{
  if (cnt_sdl >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");

  s_SdlItem item;
  item.eventType = eventType;
  item.func = func;

  hnd_sdl[cnt_sdl++] = item;
}

void AttachResetHandler(void (* func)(void))
{
  if (cnt_reset >= MAX_HANDLERS) StrikeError("Increase MAX_HANDLERS");
  hnd_reset[cnt_reset++] = func;
}

//--------------------------------------------------------------------------------------------------

C_Border dev_border;
C_ExtPort dev_extport;
C_Keyboard dev_keyboard;
C_TrDos dev_trdos;
C_MemoryManager dev_mman;
C_TsFm dev_tsfm;
C_Mouse dev_mouse;
C_Covox dev_covox;
C_KempstonStick dev_kempston; // [boo_boo]
C_GSound dev_gsound;

C_Device *devs[] =
{
  &dev_border,
  &dev_extport,
  &dev_keyboard,
  &dev_trdos,
  &dev_mman,
  &dev_tsfm,
  &dev_mouse,
  &dev_covox,
  &dev_kempston, // [boo_boo]
  &dev_gsound,
  nullptr
};

int colors_base[0x10] = {
  DRGB(0,   0,   0),
  DRGB(0,   0, 192),
  DRGB(192,   0,   0),
  DRGB(192,   0, 192),
  DRGB(0, 192,   0),
  DRGB(0, 192, 192),
  DRGB(192, 192,   0),
  DRGB(192, 192, 192),
  DRGB(0,   0,   0),
  DRGB(0,   0, 255),
  DRGB(255,   0,   0),
  DRGB(255,   0, 255),
  DRGB(0, 255,   0),
  DRGB(0, 255, 255),
  DRGB(255, 255,   0),
  DRGB(255, 255, 255)
};

/*
 * Work in progress
 *
int c64_colors_base[0x10] = {
	DRGB(0x00, 0x00, 0x00),
	DRGB(0x35, 0x28, 0x79),
	DRGB(0x9A, 0x67, 0x59),
	DRGB(0x6F, 0x3D, 0x86),
	DRGB(0x58, 0x8D, 0x43),
	DRGB(0x70, 0xA4, 0xB2),
	DRGB(0xB8, 0xC7, 0x6F),
	DRGB(0x95, 0x95, 0x95),

	DRGB(0x00, 0x00, 0x00),
	DRGB(0x35, 0x28, 0x79),
	DRGB(0x9A, 0x67, 0x59),
	DRGB(0x6F, 0x3D, 0x86),
	DRGB(0x58, 0x8D, 0x43),
	DRGB(0x70, 0xA4, 0xB2),
	DRGB(0xB8, 0xC7, 0x6F),
	DRGB(0xFF, 0xFF, 0xFF)
};
*/

int colors[0x10];

//--------------------------------------------------------------------------------------------------

int messageTimeout = 0;
char message[0x100];

void OutputText(char *str)
{
  int x, y;

  x = (WIDTH - font.StrLenPx(str)) / 2;
  y = HEIGHT - font.Height() - 4;

  font.PrintString(x, y, str);
}

void ShowMessage(void)
{
  if (messageTimeout <= 0) return;
  messageTimeout--;
  OutputText(message);
}

void SetMessage(const char *str)
{
  strcpy(message, str);
  messageTimeout = 50;
}

//--------------------------------------------------------------------------------------------------

// void TryFreeLongImage(void)
// {
// 	if (!isLongImageWrited) {
// 		return;
// 	}
//
// 	longImageFile.Close();
//
// 	longImageFile.Write(longImageFileName);
// 	longImageFile.PrintF("P6\n%ld %ld\n255\n", longImageWidth, longImageHeight);
//
// 	C_File longImageTmp;
//
// 	longImageTmp.Read("longimage.ppm.tmp");
// 	while (!longImageTmp.Eof()) longImageFile.PutBYTE(longImageTmp.GetBYTE());
//
// 	longImageTmp.Close();
// 	longImageFile.Close();
//
// 	unlink("longimage.ppm.tmp");
// 	isLongImageWrited = false;
// }

void TryFreeAvgImage(void)
{
  if (!isAvgImageWrited) {
    return;
  }

  C_File avgImageFile;
  avgImageFile.Write(avgImageFileName);
  avgImageFile.PrintF("P6\n%ld %ld\n255\n", WIDTH, HEIGHT);

  long divider = 0;
  long *ptr = avgImageBuffer;

  for (int i = WIDTH * HEIGHT * 3; i--;)
  {
    long val = *(ptr++);

    if (val > divider) {
      divider = val;
    }
  }

  divider /= 255;

  if (divider < 1) {
    divider = 1;
  }

  // long divider = (avgImageFrames > 0 ? avgImageFrames : 1);
  ptr = avgImageBuffer;

  for (int i = WIDTH * HEIGHT * 3; i--;) {
    avgImageFile.PutBYTE(*(ptr++) / divider);
  }

  avgImageFile.Close();

  delete[] avgImageBuffer;
  avgImageBuffer = nullptr;

  isAvgImageWrited = false;
}

//--------------------------------------------------------------------------------------------------

void ResetSequence(void);

void StrToLower(char *str)
{
  while (*str) {
    *(str) = tolower(*str);
    str++;
  }
}

#define MAX_FILES 4096
#define MAX_FNAME 256

void LoadNormalFile(const char *fname, int drive, const char *arcName = nullptr)
{
  if (C_Tape::IsTapeFormat(fname))
  {
    C_Tape::Insert(fname);
  }
  else if (!stricmp(C_DirWork::ExtractExt(fname), "z80"))
  {
    if (load_z80_snap(fname, cpu, dev_mman, dev_border)) {
      dev_tsfm.OnReset();
    } else {
      StrikeMessage("Error loading snapshot");
    }
  }
  else if (!stricmp(C_DirWork::ExtractExt(fname), "sna"))
  {
    if (load_sna_snap(fname, cpu, dev_mman, dev_border)) {
      dev_tsfm.OnReset();
    } else {
      StrikeMessage("Error loading snapshot");
    }
  }
  else
  {
    wd1793_load_dimage(fname, drive);
    strcpy(oldFileName[drive], C_DirWork::Normalize(arcName ? arcName : fname, true));
  }
}

bool TryLoadArcFile(const char *arcName, int drive)
{
  C_File fl;
  char res[MAX_PATH];
  char tmp[MAX_PATH], str[MAX_FNAME];
  char files[MAX_FILES][MAX_FNAME];
  int filesCount;
  string plugin_fn;

  strcpy(tmp, C_DirWork::ExtractExt(arcName));

  if (!strlen(tmp)) {
    return false;
  }

  StrToLower(tmp);

  plugin_fn = env.FindDataFile("arc", tmp);

  if (plugin_fn.empty()) {
    return false;
  }

  sprintf(tmp, "%s list \"%s\" %s/files.txt", plugin_fn.c_str(), arcName, tempFolderName);
  if (system(tmp) == -1) DEBUG_MESSAGE("system failed");

  sprintf(tmp, "%s/files.txt", tempFolderName);
  if (!C_File::FileExists(tmp)) return true; // "true" here means ONLY that the file is an archive

  fl.Read(tmp);

  for (filesCount = 0; !fl.Eof();)
  {
    fl.GetS(str, sizeof(str));
    if (strcmp(str, "")) strcpy(files[filesCount++], str);
  }

  unlink(tmp);
  if (!filesCount) return true; // "true" here means ONLY that the file is an archive

  // currently load only first file
  sprintf(tmp, "%s extract \"%s\" \"%s\" %s", plugin_fn.c_str(), arcName, files[0], tempFolderName);
  if (system(tmp) == -1) DEBUG_MESSAGE("system failed");
  strcpy(tmp, C_DirWork::ExtractFileName(files[0]));

  // TODO: check if lresult strlen > sizeof(res)
  sprintf(res, "%s/%s", tempFolderName, tmp);

  LoadNormalFile(res, drive, arcName);
  unlink(res);

  return true; // "true" here means ONLY that the file is an archive
}

void TryNLoadFile(const char *fname, int drive)
{
  char bname[MAX_PATH];
  strcpy(bname, fname);

  char *tname = bname;

  if (tname[0] == '"') {
    tname++;
    int len = strlen(tname);

    if (tname[len - 1] == '"') {
      tname[len - 1] = '\0';
    }
  }

#ifdef _WIN32
  if (tname[0] == '/' && tname[2] == '/')
  {
    tname[0] = tname[1];
    tname[1] = ':';
  }
#endif

  if (tname[0] != 0) {
    printf("Trying to load \"%s\" ...\n", tname);

    if (!TryLoadArcFile(tname, drive)) {
      LoadNormalFile(tname, drive);
    }
  }
}

//--------------------------------------------------------------------------------------------------

void Action_Reset(void)
{
  isPaused = false;
  ResetSequence();
  dev_gsound.Reset();
}

void Action_ResetTrDos(void)
{
  isPaused = false;
  ResetSequence();
  dev_mman.OnOutputByte(0x7FFD, 0x10);
  dev_trdos.Enable();
}

void Action_MaxSpeed(void)
{
  isPaused = false;
  params.maxSpeed = !params.maxSpeed;
  SetMessage(params.maxSpeed ? "MaxSpeed ON" : "MaxSpeed OFF");
}

void Action_QuickLoad(void)
{
  bool (* loadSnap)(const char *filename, Z80EX_CONTEXT * cpu, C_MemoryManager & mmgr,
                    C_Border & border);

  isPaused = false;

  const char *snapName = (params.snapFormat == SNAP_FORMAT_SNA ? "snap.sna" : "snap.z80");
  loadSnap = (params.snapFormat == SNAP_FORMAT_SNA ? load_sna_snap : load_z80_snap);

  if (!loadSnap(snapName, cpu, dev_mman, dev_border)) SetMessage("Error loading snapshot");
  else SetMessage("Snapshot loaded");
}

void Action_QuickSave(void)
{
  void (* saveSnap)(const char *filename, Z80EX_CONTEXT * cpu, C_MemoryManager & mmgr,
                    C_Border & border);

  isPaused = false;

  const char *snapName = (params.snapFormat == SNAP_FORMAT_SNA ? "snap.sna" : "snap.z80");
  saveSnap = (params.snapFormat == SNAP_FORMAT_SNA ? save_sna_snap : save_z80_snap);

  saveSnap(snapName, cpu, dev_mman, dev_border);
  SetMessage("Snapshot saved");
}

void Action_AntiFlicker(void)
{
  isPaused = false;
  params.antiFlicker = !params.antiFlicker;
  if (params.antiFlicker) doCopyOfSurfaces = true;
  SetMessage(params.antiFlicker ? "AntiFlicker ON" : "AntiFlicker OFF");
}

void Action_LoadFile(void)
{
  FileDialog();
}

void Action_Fullscreen(void)
{
#ifdef __unix__
  SDL_WM_ToggleFullScreen(realScreen);
#else
  videoSpec ^= SDL_FULLSCREEN;
  SDL_FreeSurface(realScreen);
  realScreen = SDL_SetVideoMode(actualWidth, actualHeight, 32, videoSpec);
  REAL_PITCH = realScreen->pitch / 4;

  if (!params.scale2x)
  {
    screen = realScreen;
    PITCH = REAL_PITCH;
  }
#endif
}

void Action_Debugger(void)
{
  isPaused = false;
  RunDebugger();
}

void Action_Turbo(void)
{
  isPaused = false;

  if (unturboNx && turboMultiplierNx > 1)
  {
    turboMultiplierNx /= 2;
    unturboNx = (turboMultiplierNx > 1);
  }
  else
  {
    unturboNx = false;
    turboMultiplierNx = (turboMultiplierNx == 8 ? 1 : (turboMultiplierNx * 2));
  }

  DisplayTurboMessage();
}

void Action_UnTurbo(void)
{
  isPaused = false;

  if (!unturboNx && turboMultiplierNx > 1)
  {
    turboMultiplierNx /= 2;
  }
  else
  {
    turboMultiplierNx = (turboMultiplierNx == 256 ? 1 : (turboMultiplierNx * 2));
    unturboNx = (turboMultiplierNx > 1);
  }

  DisplayTurboMessage();
}

void DisplayTurboMessage(void)
{
  if (turboMultiplierNx < 2)
  {
    SetMessage("Turbo OFF");
  }
  else
  {
    char buf[0x10];
    sprintf(buf, (unturboNx ? "Slowness %dx" : "Turbo %dx"), turboMultiplierNx);
    SetMessage(buf);
  }
}

void Action_AttributesHack(void)
{
  isPaused = false;
  attributesHack = (attributesHack + 1) % 3;

  if (attributesHack) {
    char buf[0x20];
    sprintf(buf, "AttributesHack #%d", attributesHack);
    SetMessage(buf);
  } else {
    SetMessage("AttributesHack OFF");
  }
}

void Action_ScreensHack(void)
{
  isPaused = false;
  screensHack = screensHack ^ 8;
  SetMessage(screensHack ? "ScreensHack ON" : "ScreensHack OFF");
}

void Action_FlashColor(void)
{
  isPaused = false;
  flashColor = !flashColor;
  SetMessage(flashColor ? "FlashColor ON" : "FlashColor OFF");
}

void Action_Pause(void)
{
  isPausedNx = !isPausedNx;
  SetMessage(isPausedNx ? "Pause ON" : "Pause OFF");
}

void Action_JoyOnKeyb(void)
{
  isPaused = false;
  joyOnKeyb = !joyOnKeyb;
  dev_kempston.joy_kbd = 0;
  SetMessage(joyOnKeyb ? "Kempston on keyboard ON" : "Kempston on keyboard OFF");
}

// void Action_WriteLongImage(void)
// {
// 	if (isLongImageWrited)
// 	{
// 		TryFreeLongImage();
// 	}
// 	else
// 	{
// 		isLongImageWrited = true;
// 		longImageFile.Write("longimage.ppm.tmp");
//
// 		longImageWidth = 256;
// 		longImageHeight = 0;
// 		longImagePos = 0;
// 	}
//
// 	SetMessage(isLongImageWrited ? "Long image write ON" : "Long image write OFF");
// }

void Action_WriteAvgImage(void)
{
  if (isAvgImageWrited)
  {
    TryFreeAvgImage();
  }
  else
  {
    isAvgImageWrited = true;
    avgImageBuffer = new long[WIDTH * HEIGHT * 3];
    // avgImageFrames = 0;

    long *ptr = avgImageBuffer;

    for (int i = WIDTH * HEIGHT * 3; i--;) {
      *(ptr++) = 0;
    }
  }

  SetMessage(isAvgImageWrited ? "Avg image write ON" : "Avg image write OFF");
}

s_Action cfgActions[] =
{
  {"reset",           Action_Reset},
  {"reset_trdos",     Action_ResetTrDos},
  {"max_speed",       Action_MaxSpeed},
  {"quick_load",      Action_QuickLoad},
  {"quick_save",      Action_QuickSave},
  {"anti_flicker",    Action_AntiFlicker},
  {"load_file",       Action_LoadFile},
  {"fullscreen",      Action_Fullscreen},
  {"debugger",        Action_Debugger},
  {"turbo",           Action_Turbo},
  {"unturbo",         Action_UnTurbo},
  {"attrib_hack",     Action_AttributesHack},
  {"screens_hack",    Action_ScreensHack},
  {"flash_color",     Action_FlashColor},
  {"pause",           Action_Pause},
  {"joy_on_keyb",     Action_JoyOnKeyb},
  // {"write_longimage", Action_WriteLongImage},
  {"write_longimage", Action_WriteAvgImage},
  {"",                nullptr}
};

//--------------------------------------------------------------------------------------------------

bool runDebuggerFlag = false;
bool breakpoints[0x10000];

uint16_t watches[MAX_WATCHES];
unsigned watchesCount = 0;

uint8_t ReadByteDasm(uint16_t addr, void *userData)
{
  ptrOnReadByteFunc func = devMapRead[addr];
  return func(addr, false);
}

void WriteByteDasm(uint16_t addr, uint8_t value)
{
  for (;;)
  {
    bool (* func)(uint16_t, uint8_t) = devMapWrite[addr];

    if (func == nullptr) return;
    if (func(addr, value)) return;
  }
}

uint8_t ReadByte(Z80EX_CONTEXT_PARAM uint16_t addr, int m1_state, void *userData)
{
  unsigned raddr = addr + (m1_state ? 0x10000 : 0);
  ptrOnReadByteFunc func = devMapRead[raddr];
  return func(addr, m1_state);
}

void WriteByte(Z80EX_CONTEXT_PARAM uint16_t addr, uint8_t value, void *userData)
{
  for (;;)
  {
    bool (* func)(uint16_t, uint8_t) = devMapWrite[addr];

    if (func == nullptr) return;
    if (func(addr, value)) return;
  }
}

uint8_t InputByte(Z80EX_CONTEXT_PARAM uint16_t port, void *userData)
{
  uint8_t retval;

  for (;;)
  {
    bool (* func)(uint16_t, uint8_t &) = devMapInput[port];

    if (func == nullptr) return 0xFF;
    if (func(port, retval)) return retval;
  }
}

void OutputByte(Z80EX_CONTEXT_PARAM uint16_t port, uint8_t value, void *userData)
{
  for (;;)
  {
    bool (* func)(uint16_t, uint8_t) = devMapOutput[port];

    if (func == nullptr) return;
    if (func(port, value)) return;
  }
}

uint8_t ReadIntVec(Z80EX_CONTEXT_PARAM void *userData)
{
  return 0xFF;
}

//--------------------------------------------------------------------------------------------------

void InitDevMapRead(ptrOnReadByteFunc *map)
{
  ptrOnReadByteFunc func;

  for (unsigned m1_state = 0; m1_state < 2; m1_state++)
  {
    for (unsigned addr = 0; addr < 0x10000; addr++)
    {
      map[addr + (m1_state ? 0x10000 : 0)] = nullptr;

      for (int i = 0; i < cnt_z80read; i++)
      {
        if ((func = hnd_z80read[i].check(addr, m1_state)) != nullptr)
        {
          map[addr + (m1_state ? 0x10000 : 0)] = func;
          break;
        }
      }
    }
  }
}

void InitDevMapWrite(bool (** map)(uint16_t, uint8_t))
{
  for (unsigned addr = 0; addr < 0x10000; addr++)
  {
    map[addr] = nullptr;

    for (int i = 0; i < cnt_z80write; i++)
    {
      if (hnd_z80write[i].check(addr))
      {
        map[addr] = hnd_z80write[i].func;
        break;
      }
    }
  }
}

void InitDevMapInput(bool (** map)(uint16_t, uint8_t &))
{
  for (unsigned port = 0; port < 0x10000; port++)
  {
    map[port] = nullptr;

    for (int i = 0; i < cnt_z80input; i++)
    {
      if (hnd_z80input[i].check(port))
      {
        map[port] = hnd_z80input[i].func;
        break;
      }
    }
  }
}

void InitDevMapOutput(bool (** map)(uint16_t, uint8_t))
{
  for (unsigned port = 0; port < 0x10000; port++)
  {
    map[port] = nullptr;

    for (int i = 0; i < cnt_z80output; i++)
    {
      if (hnd_z80output[i].check(port))
      {
        map[port] = hnd_z80output[i].func;
        break;
      }
    }
  }
}

void InitDevMaps(void)
{
  C_TrDos::trdos = true;

  InitDevMapRead(devMapRead_trdos);
  InitDevMapInput(devMapInput_trdos);
  InitDevMapOutput(devMapOutput_trdos);

  C_TrDos::trdos = false;

  InitDevMapRead(devMapRead_base);
  InitDevMapWrite(devMapWrite_base);
  InitDevMapInput(devMapInput_base);
  InitDevMapOutput(devMapOutput_base);

  devMapRead = devMapRead_base;
  devMapWrite = devMapWrite_base;
  devMapInput = devMapInput_base;
  devMapOutput = devMapOutput_base;
}

//--------------------------------------------------------------------------------------------------

void InitSurfaces(void)
{
  SDL_PixelFormat *fmt = screen->format;

  scrSurf[0] = SDL_CreateRGBSurface(SDL_SWSURFACE, WIDTH, HEIGHT, fmt->BitsPerPixel, fmt->Rmask,
                                    fmt->Gmask, fmt->Bmask, 0);
  if (scrSurf[0] == nullptr)
    StrikeError("Unable to create primary surface: %s\n", SDL_GetError());

  scrSurf[1] = SDL_CreateRGBSurface(SDL_SWSURFACE, WIDTH, HEIGHT, fmt->BitsPerPixel, fmt->Rmask,
                                    fmt->Gmask, fmt->Bmask, 0);
  if (scrSurf[1] == nullptr)
    StrikeError("Unable to create secondary surface: %s\n", SDL_GetError());
}

void InitFont(void)
{
  font.Init(ftbos_font_data);
  font.CopySym('-', '_');
  font.SetSymOff('_', 0, 4);
  font.SetSymOff('-', 0, 1);

  fixed_font.Init(font_64_data);
}

void InitAll(void)
{
  int i;
  for (i = 0; devs[i]; i++) devs[i]->Init();

  InitDevMaps();

  for (i = 0; i < 0x10; i++) colors[i] = colors_base[i];

  InitSurfaces();
  InitFont();
  FileDialogInit();
  C_Tape::Init();

  for (i = 0; i < 0x10000; i++) breakpoints[i] = false;

  cpu = z80ex_create(
          ReadByte, nullptr,
          WriteByte, nullptr,
          InputByte, nullptr,
          OutputByte, nullptr,
          ReadIntVec, nullptr
        );

#if defined(__APPLE__)
  updateScreenThreadSem = SDL_CreateSemaphore(0);
  upadteScreenThread = SDL_CreateThread(UpdateScreenThreadFunc, nullptr);
#endif
}

#define INT_LENGTH 32

int attributesHack = 0;
bool flashColor = false;
int screensHack = 0;

// ----------------------------------

void AntiFlicker(SDL_Surface *copyFrom, SDL_Surface *copyTo)
{
  int i, j;
  uint8_t *s1, *s2, *sr;
  uint8_t *s1w, *s2w, *srw;

  if (SDL_MUSTLOCK(screen)) {
    if (SDL_LockSurface(screen) < 0) return;
  }
  if (SDL_MUSTLOCK(scrSurf[0])) {
    if (SDL_LockSurface(scrSurf[0]) < 0) return;
  }
  if (SDL_MUSTLOCK(scrSurf[1])) {
    if (SDL_LockSurface(scrSurf[1]) < 0) return;
  }

  if (doCopyOfSurfaces)
  {
    s1 = (uint8_t *)copyFrom->pixels;
    s2 = (uint8_t *)copyTo->pixels;

    for (i = HEIGHT; i--;)
    {
      uint32_t *s1dw = (uint32_t *)s1;
      uint32_t *s2dw = (uint32_t *)s2;

      for (j = WIDTH; j--;) {
        *(s2dw++) = *(s1dw++);
      }

      s1 += copyFrom->pitch;
      s2 += copyTo->pitch;
    }

    doCopyOfSurfaces = false;
  }

  sr = (uint8_t *)screen->pixels;
  s1 = (uint8_t *)scrSurf[0]->pixels;
  s2 = (uint8_t *)scrSurf[1]->pixels;

  for (i = HEIGHT; i--;)
  {
    srw = sr;
    s1w = s1;
    s2w = s2;

    for (j = WIDTH; j--;)
    {
#if defined(ZEMU_BIG_ENDIAN) || defined(__APPLE__)
      *(srw++) = 0;
      s1w++;
      s2w++;
#endif

      *srw = (uint8_t)(((unsigned int)(*s1w) + (unsigned int)(*s2w)) >> 1);
      srw++;
      s1w++;
      s2w++;

      *srw = (uint8_t)(((unsigned int)(*s1w) + (unsigned int)(*s2w)) >> 1);
      srw++;
      s1w++;
      s2w++;

      *srw = (uint8_t)(((unsigned int)(*s1w) + (unsigned int)(*s2w)) >> 1);
      srw++;
      s1w++;
      s2w++;

#if !defined(ZEMU_BIG_ENDIAN) && !defined(__APPLE__)
      *(srw++) = 0;
      s1w++;
      s2w++;
#endif
    }

    sr += screen->pitch;
    s1 += scrSurf[0]->pitch;
    s2 += scrSurf[1]->pitch;
  }

  if (SDL_MUSTLOCK(scrSurf[1])) SDL_UnlockSurface(scrSurf[1]);
  if (SDL_MUSTLOCK(scrSurf[0])) SDL_UnlockSurface(scrSurf[0]);
  if (SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
}

uint64_t actDevClkCounter = 0;
uint64_t actClk = 0;

void InitActClk(void)
{
  actDevClkCounter = devClkCounter * (uint64_t)turboMultiplier;
  actClk = cpuClk * (uint64_t)turboMultiplier;
}

// TODO:
// CpuCalcTacts - pointer to function
// CpuCalcTacts_Normal
// CpuCalcTacts_Turbo
// CpuCalcTacts_Unturbo
// DebugCpuCalcTacts_Normal
// ...
// ...
// TODO: refactor

inline void CpuCalcTacts(unsigned long cmdClk) {
  if (turboMultiplier < 2) {
    devClkCounter += (uint64_t)cmdClk;
    cpuClk += (uint64_t)cmdClk;
  } else if (unturbo) {
    cmdClk *= turboMultiplier;
    devClkCounter += (uint64_t)cmdClk;
    cpuClk += (uint64_t)cmdClk;
  } else {
    actDevClkCounter += (uint64_t)cmdClk;
    actClk += (uint64_t)cmdClk;

    devClkCounter = (actDevClkCounter + (uint64_t)(turboMultiplier - 1)) /
                                        (uint64_t)turboMultiplier;
    cpuClk = (actClk + (uint64_t)(turboMultiplier - 1)) / (uint64_t)turboMultiplier;
  }

  devClk = cpuClk;
  C_Tape::Process();

  if (runDebuggerFlag || breakpoints[z80ex_get_reg(cpu, regPC)]) {
    if (SDL_MUSTLOCK(renderSurf)) {
      SDL_UnlockSurface(renderSurf);
    }

    runDebuggerFlag = false;
    RunDebugger();

    if (SDL_MUSTLOCK(renderSurf)) {
      if (SDL_LockSurface(renderSurf) < 0) {
        printf("Can't lock surface\n");
        return;
      }
    }
  }
}

inline void DebugCpuCalcTacts(unsigned long cmdClk)
{
  if (turboMultiplier < 2)
  {
    devClkCounter += (uint64_t)cmdClk;
    cpuClk += (uint64_t)cmdClk;
  }
  else if (unturbo)
  {
    cmdClk *= turboMultiplier;
    devClkCounter += (uint64_t)cmdClk;
    cpuClk += (uint64_t)cmdClk;
  }
  else
  {
    actDevClkCounter += (uint64_t)cmdClk;
    actClk += (uint64_t)cmdClk;

    devClkCounter = (actDevClkCounter + (uint64_t)(turboMultiplier - 1)) /
                                        (uint64_t)turboMultiplier;
    cpuClk = (actClk + (uint64_t)(turboMultiplier - 1)) / (uint64_t)turboMultiplier;
  }

  devClk = cpuClk;
  C_Tape::Process();
}

int (* DoCpuStep)(Z80EX_CONTEXT *cpu) = z80ex_step;
int (* DoCpuInt)(Z80EX_CONTEXT *cpu) = z80ex_int;

int TraceCpuStep(Z80EX_CONTEXT *cpu)
{
  CpuTrace_Log();
  cpuTrace_intReq = 0;
  cpuTrace_dT = z80ex_step(cpu);
  return cpuTrace_dT;
}

int TraceCpuInt(Z80EX_CONTEXT *cpu)
{
  CpuTrace_Log();
  int dt = z80ex_int(cpu);
  cpuTrace_dT += dt;
  cpuTrace_intReq++;
  return dt;
}

inline void CpuStep(void)
{
  CpuCalcTacts(DoCpuStep(cpu));
}

inline void CpuInt(void)
{
  CpuCalcTacts(DoCpuInt(cpu));
}

inline void DebugCpuStep(void)
{
  DebugCpuCalcTacts(DoCpuStep(cpu));
}

inline void DebugCpuInt(void)
{
  DebugCpuCalcTacts(DoCpuInt(cpu));
}

void DebugStep(void)
{
  int cnt = 4;

  do
  {
    if (cpuClk < MAX_FRAME_TACTS)
    {
      CpuStep();
      if (cpuClk < INT_LENGTH) CpuInt();
    }
    else
    {
      lastDevClk = devClk;
      cpuClk -= MAX_FRAME_TACTS;
      devClk = cpuClk;

      InitActClk();
    }

    cnt--;
  } while (z80ex_last_op_type(cpu) && cnt > 0);
}

SDL_Surface *renderSurf;
int renderPitch;
unsigned long prevRenderClk;
void (* renderPtr)(unsigned long) = nullptr;

void Render(void)
{
  static int sn = 0;

  if (params.antiFlicker)
  {
    renderSurf = scrSurf[sn];
    sn = 1 - sn;
  } else renderSurf = screen;

  if ((drawFrame || isAvgImageWrited /* isLongImageWrited */) && SDL_MUSTLOCK(renderSurf))
  {
    if (SDL_LockSurface(renderSurf) < 0)
    {
      printf("Can't lock surface\n");
      return;
    }
  }

  renderPitch = renderSurf->pitch / 4;

  if (dev_extport.Is16Colors()) renderPtr = Render16c;
  else if (dev_extport.IsMulticolor()) renderPtr = RenderMulticolor;
//  else if (dev_extport.Is512x192()) renderPtr = Render512x192;
//  else if (dev_extport.Is384x304()) renderPtr = Render384x304;
  else renderPtr = RenderSpeccy;

  InitActClk();
  prevRenderClk = 0;

  while (cpuClk < INT_LENGTH)
  {
    CpuStep();
    CpuInt();
  }

  if (drawFrame || isAvgImageWrited /* isLongImageWrited */)
  {
    while (cpuClk < MAX_FRAME_TACTS)
    {
      CpuStep();
      renderPtr(cpuClk);
    }
  }
  else
  {
    while (cpuClk < MAX_FRAME_TACTS) {
      CpuStep();
    }
  }

  renderPtr = nullptr;
  lastDevClk = devClk;
  cpuClk -= MAX_FRAME_TACTS;
  devClk = cpuClk;

  if ((drawFrame || isAvgImageWrited /* isLongImageWrited */) && SDL_MUSTLOCK(renderSurf))
    SDL_UnlockSurface(renderSurf);
  if (params.antiFlicker && (drawFrame || isAvgImageWrited /* isLongImageWrited */))
    AntiFlicker(renderSurf, scrSurf[sn]);

  // if (isLongImageWrited)
  // {
  //  int * src = (int *)screen->pixels + (PITCH * (32 + longImagePos)) + 32;
  //
  //  for (int i = 256; i--;)
  //  {
  //    unsigned int c = *src;
  //    int r = GETR(c);
  //    int g = GETG(c);
  //    int b = GETB(c);
  //
  //    longImageFile.PutBYTE(r);
  //    longImageFile.PutBYTE(g);
  //    longImageFile.PutBYTE(b);
  //
  //    *(src++) = DRGB(255 - r, 255 - g, 255 - b);
  //  }
  //
  //  longImageHeight++;
  //  longImagePos = (longImagePos + 1) % 192;
  // }

  if (isAvgImageWrited)
  {
    int *src = (int *)screen->pixels;
    long *dst = avgImageBuffer;

    for (int i = HEIGHT; i--;)
    {
      int *line = src;

      for (int j = WIDTH; j--;)
      {
        unsigned int c = *(line++);

        *(dst++) += (long)GETR(c);
        *(dst++) += (long)GETG(c);
        *(dst++) += (long)GETB(c);
      }

      src += PITCH;
    }

    // avgImageFrames++;
  }
}

#include "images/floppy.h"
#include "images/floppy_read.h"
#include "images/floppy_write.h"
#include "images/turbo_off.h"
#include "images/turbo_on.h"

void DrawIndicators(void)
{
  char buf[0x100];

  DRIVE_STATE st = dev_trdos.GetIndicatorState();

  if (st == DS_READ) OutputGimpImage(8, 0, (s_GimpImage *) &img_floppyRead);
  else if (st == DS_WRITE) OutputGimpImage(8, 0, (s_GimpImage *) &img_floppyWrite);
  else if (params.showInactiveIcons) OutputGimpImage(8, 0, (s_GimpImage *) &img_floppy);

  if (params.maxSpeed) OutputGimpImage(32, 0, (s_GimpImage *) &img_turboOn);
  else if (params.showInactiveIcons) OutputGimpImage(32, 0, (s_GimpImage *) &img_turboOff);

  if (C_Tape::IsActive())
  {
    sprintf(buf, "%d%%", C_Tape::GetPosPerc());

    int wdt = font.StrLenPx(buf);
    font.PrintString(WIDTH - 4 - wdt, 4, buf);
  }

  for (unsigned i = 0; i < watchesCount; i++)
  {
    uint8_t val = ReadByteDasm(watches[i], nullptr);
    sprintf(buf, "%04X:%02X", watches[i], val);

    int wdt = fixed_font.StrLenPx(buf);
    fixed_font.PrintString(WIDTH - 4 - wdt, 4 + fixed_font.Height() * (i + 1), buf);
  }
}

void ResetSequence(void)
{
  int cnt = cnt_reset;
  void (** ptr)(void) = hnd_reset;

  z80ex_reset(cpu);

  while (cnt)
  {
    (*ptr)();

    ptr++;
    cnt--;
  }
}

void Process(void)
{
  int key;
  SDL_Event event;
  int i;
  unsigned int btick;
  int frameSkip = 0;
  bool tapePrevActive = false;

  devClkCounter = 0;
  cpuClk = 0;
  devClk = 0;
  lastDevClk = 0;
  frames = 0;
  params.maxSpeed = false;

  btick = SDL_GetTicks() + (params.sound ? 0 : FRAME_WAIT_MS);
  //*/ unsigned long lastDivider = 0L;

  for (;;)
  {
    if (!isPaused)
    {
      i = cnt_frameStart;
      void (** ptr_frameStart)(void) = hnd_frameStart;

      while (i)
      {
        (*ptr_frameStart)();

        ptr_frameStart++;
        i--;
      }

      if (params.maxSpeed)
      {
        if (frameSkip > 0) {
          frameSkip--;
          drawFrame = false;
        } else {
          frameSkip = MAX_SPEED_FRAME_SKIP;
          drawFrame = true;
        }
      } else drawFrame = true;

      tapePrevActive = C_Tape::IsActive();

      Render();
      frames++;

      if (drawFrame)
      {
        DrawIndicators();
        ShowMessage();

        /*//
        char bbb[0x100]; sprintf(bbb, "%ld", lastDivider); font.PrintString(0, 0, bbb);
        for (i = 0; i < cnt_sndRenderers; i++) fixed_font.PrintString(16+i*4, 0, sndRenderers[i]->activeCnt ? "#" : "-");
        //*/

        UpdateScreen();
      }

      if (!params.maxSpeed)
      {
        SDL_Delay(1);
        if (SDL_GetTicks() < btick) SDL_Delay(btick - SDL_GetTicks());
      }

      i = cnt_afterFrameRender;
      void (** ptr_afterFrameRender)(void) = hnd_afterFrameRender;

      while (i)
      {
        (*ptr_afterFrameRender)();

        ptr_afterFrameRender++;
        i--;
      }

      soundMixer.FlushFrame(SOUND_ENABLED);
    }

    btick = SDL_GetTicks() + (params.sound ? 0 : FRAME_WAIT_MS);

    isPaused = isPausedNx;
    bool quitMode = false;

    while (SDL_PollEvent(&event))
    {
      if (event.type == SDL_QUIT) exit(0);

      if (event.type == SDL_KEYUP)
      {
        key = event.key.keysym.sym;
        if (key == SDLK_ESCAPE) quitMode = true;
      }

      i = cnt_sdl;
      s_SdlItem *ptr_sdl = hnd_sdl;

      while (i)
      {
        if (ptr_sdl->eventType == event.type) {
          if (ptr_sdl->func(event)) {
            break;
          }
        }

        ptr_sdl++;
        i--;
      }
    }

    if (quitMode)
    {
      isPaused = false;

      if (DlgConfirm("Do you really want to quit?")) {
        return;
      }
    }

    turboMultiplier = turboMultiplierNx;
    unturbo = unturboNx;

    if (!isPaused && tapePrevActive && !C_Tape::IsActive())
    {
      if (params.maxSpeed)
      {
        SetMessage("Tape end : MaxSpeed OFF");
        params.maxSpeed = false;
      }
      else SetMessage("Tape end");
    }
  }
}

void InitAudio(void)
{
  if (params.sndBackend == SND_BACKEND_SDL) {
    soundMixer.InitBackendSDL(params.sdlBufferSize);
  }
#if !defined(_WIN32) && !defined(__APPLE__)
  else if (params.sndBackend == SND_BACKEND_OSS) {
    soundMixer.InitBackendOSS(params.soundParam);
  }
#else
#ifdef _WIN32
  else if (params.sndBackend == SND_BACKEND_WIN32) {
    soundMixer.InitBackendWin32(params.soundParam);
  }
#endif
#endif // !_WIN32 && !__APPLE__

  soundMixer.Init(params.mixerMode, recordWav, wavFileName);
}

#if defined(__APPLE__)
int UpdateScreenThreadFunc(void *param)
{
  while (updateScreenThreadActive) {
    SDL_SemWait(updateScreenThreadSem);

    if (params.useFlipSurface) SDL_Flip(realScreen);
    else SDL_UpdateRect(realScreen, 0, 0, 0, 0);
  }

  return 0;
}
#endif

void UpdateScreen(void)
{
  if (!params.scale2x) {
    // do not use threading here, because realScreen == screen

    if (params.useFlipSurface) SDL_Flip(realScreen);
    else SDL_UpdateRect(realScreen, 0, 0, 0, 0);

    return;
  }

#if defined(__APPLE__)
  if (SDL_SemValue(updateScreenThreadSem)) {
    return;
  }
#endif

  if (SDL_MUSTLOCK(realScreen)) {
    if (SDL_LockSurface(realScreen) < 0) return;
  }

  if (SDL_MUSTLOCK(screen)) {
    if (SDL_LockSurface(screen) < 0) {
      if (SDL_MUSTLOCK(realScreen)) SDL_UnlockSurface(realScreen);
      return;
    }
  }

  if (params.scanlines)
  {
    for (int i = HEIGHT - 1; i >= 0; i--)
    {
      int *line = (int *)screen->pixels + i * PITCH + WIDTH - 1;
      int *lineA = (int *)realScreen->pixels + (i * 2) * REAL_PITCH + (WIDTH * 2 - 1);
      int *lineB = (int *)realScreen->pixels + (i * 2 + 1) * REAL_PITCH + (WIDTH * 2 - 1);

      for (int j = WIDTH; j--;)
      {
        int c = *(line--);
        int dc = (c & 0xFEFEFE) >> 1;

        *(lineA--) = c;
        *(lineA--) = c;
        *(lineB--) = dc;
        *(lineB--) = dc;
      }
    }
  }
  else
  {
    for (int i = HEIGHT - 1; i >= 0; i--)
    {
      int *line = (int *)screen->pixels + i * PITCH + WIDTH - 1;
      int *lineA = (int *)realScreen->pixels + (i * 2) * REAL_PITCH + (WIDTH * 2 - 1);
      int *lineB = (int *)realScreen->pixels + (i * 2 + 1) * REAL_PITCH + (WIDTH * 2 - 1);

      for (int j = WIDTH; j--;)
      {
        int c = *(line--);

        *(lineA--) = c;
        *(lineA--) = c;
        *(lineB--) = c;
        *(lineB--) = c;
      }
    }
  }

  if (SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
  if (SDL_MUSTLOCK(realScreen)) SDL_UnlockSurface(realScreen);

#if defined(__APPLE__)
  SDL_SemPost(updateScreenThreadSem);
#else
  if (params.useFlipSurface) SDL_Flip(realScreen);
  else SDL_UpdateRect(realScreen, 0, 0, 0, 0);
#endif
}

void FreeAll(void)
{
  int i;
  for (i = 0; devs[i]; i++) devs[i]->Close();

  C_Tape::Close();

#if defined(__APPLE__)
  if (upadteScreenThread) {
    updateScreenThreadActive = false;
    SDL_SemPost(updateScreenThreadSem);
    SDL_WaitThread(upadteScreenThread, nullptr);
  }
#endif

  // TryFreeLongImage();
  TryFreeAvgImage();

#ifdef __unix__
  // TODO: do in in more portable way
  char cmd[MAX_PATH];
  strcpy(cmd, "rm -rf ");
  strcat(cmd, tempFolderName);
  if (system(cmd) == -1) DEBUG_MESSAGE("system() failed");
#endif

  if (videoSpec & SDL_FULLSCREEN)
  {
    videoSpec ^= SDL_FULLSCREEN;
    SDL_FreeSurface(realScreen);
    realScreen = SDL_SetVideoMode(actualWidth, actualHeight, 32, videoSpec);
  }

  SDL_FreeSurface(scrSurf[0]);
  SDL_FreeSurface(scrSurf[1]);
  SDL_FreeSurface(realScreen);

  if (params.scale2x) SDL_FreeSurface(screen);

  z80ex_destroy(cpu);

  if (AppEnv::executableDir) {
    free(AppEnv::executableDir);
  }
}

void ParseCmdLine(int argc, char *argv[])
{
  argv++;
  argc--;

  while (argc > 0)
  {
    if (!strcmp(*argv, "-l"))
    {
      if (argc > 1)
      {
        argv++;
        argc--;

        Labels_Load(*argv);
      }
    }
    else if (!strcmp(*argv, "-w"))
    {
      recordWav = true;
    }
    else
    {
      TryNLoadFile(*argv);
      return;
    }

    argv++;
    argc--;
  }
}

void OutputLogo(void)
{
  printf("                                        \n");
  printf("    $ww,.                               \n");
  printf("     `^$$$ww,.                          \n");
  printf("        `^$$$$$$                        \n");
  printf("          ,$$7'                         \n");
  printf("        _j$$'   __    __  _ _           \n");
  printf("      ,j$$7      /__ (-_ | ) ) (_|      \n");
  printf("     $$$$$$w.                           \n");
  printf("      `^^T$$$w,             rst'o6      \n");
  printf("            `^T$$                       \n");
  printf("                                        \n");
  printf(" restorer     [ restorer.fct@gmail.com ]\n");
  printf(" boo_boo      [    boo_boo@inbox.ru    ]\n");
  printf(" mastermind   [   zxmmind@gmail.com    ]\n");
  printf("                                        \n");
  printf(" breeze (gfx) [  breeze.fbn@gmail.com  ]\n");
  printf("                                        \n");
  printf(" with help of SMT                       \n");
  printf("                                        \n");
}

#ifdef _WIN32

HICON windows_icon;
HWND hwnd;

#include <SDL_syswm.h>
#include "windows/resource.h"

void windows_init()
{
  HINSTANCE handle = ::GetModuleHandle(nullptr);
  windows_icon = ::LoadIcon(handle, MAKEINTRESOURCE(IDI_ICON1));

  if (windows_icon == nullptr) {
    StrikeError("Error: %d\n", GetLastError());
  }

  SDL_SysWMinfo wminfo;
  SDL_VERSION(&wminfo.version);

  if (int sdl_error = SDL_GetWMInfo(&wminfo) != 1) {
    StrikeError("SDL_GetWMInfo() returned %d\n", sdl_error);
  }

  hwnd = wminfo.window;
  ::SetClassLongPtr(hwnd, GCLP_HICON, (LONG_PTR)windows_icon);
}

void windows_cleanup()
{
  SDL_Quit();
  ::DestroyIcon(windows_icon);
}

#endif // _WIN32

int main(int argc, char *argv[])
{
  OutputLogo();

  if (argc > 0) {		// just for case
    AppEnv::executableDir = AllocNstrcpy(C_DirWork::ExtractPath(argv[0]));
  } else {
    AppEnv::executableDir = AllocNstrcpy(".");
  }

  env.Initialize("zemu");

  try
  {
    string str;

    // core
    str = env.GetString("core", "snapformat", "sna");
    transform(str.begin(), str.end(), str.begin(), (int(*)(int))tolower);

    if (str == "sna") params.snapFormat = SNAP_FORMAT_SNA;
    else params.snapFormat = SNAP_FORMAT_Z80;

    // beta128
    str = env.GetString("beta128", "diskA", "");
    if (!str.empty()) wd1793_load_dimage(str.c_str(), 0);

    str = env.GetString("beta128", "diskB", "");
    if (!str.empty()) wd1793_load_dimage(str.c_str(), 1);

    str = env.GetString("beta128", "diskC", "");
    if (!str.empty()) wd1793_load_dimage(str.c_str(), 2);

    str = env.GetString("beta128", "diskD", "");
    if (!str.empty()) wd1793_load_dimage(str.c_str(), 3);

    wd1793_set_nodelay(env.GetBool("beta128", "nodelay", false));

    str = env.GetString("beta128", "sclboot", "boot.$b");

    str = env.FindDataFile("boot", str.c_str());
    if (!str.empty()) wd1793_set_appendboot(str.c_str());

    // display
    params.fullscreen = env.GetBool("display", "fullscreen", false);
    params.scale2x = env.GetBool("display", "scale2x", true);
    params.scanlines = env.GetBool("display", "scanlines", false);
    params.useFlipSurface = env.GetBool("display", "sdl_useflipsurface", false);
    params.antiFlicker = env.GetBool("display", "antiflicker", false);
    params.showInactiveIcons = env.GetBool("display", "showinactiveicons", false);

    // input
    params.mouseDiv = env.GetInt("input", "mousediv", 1);
    if (params.mouseDiv <= 0) params.mouseDiv = 1;
    else if (params.mouseDiv > 8) params.mouseDiv = 8;

    // sound
    params.sound = env.GetBool("sound", "enable", true);
    params.mixerMode = env.GetInt("sound", "mixermode", 1);

#ifdef _WIN32
    eSndBackend default_snd_backend = SND_BACKEND_WIN32;
#else
    eSndBackend default_snd_backend = SND_BACKEND_SDL;
#endif

    str = env.GetString("sound", "sound_backend", "auto");
    transform(str.begin(), str.end(), str.begin(), (int(*)(int))tolower);

    if (str == "sdl") params.sndBackend = SND_BACKEND_SDL;
#if !defined(_WIN32) && !defined(__APPLE__)
    else if (str == "oss") params.sndBackend = SND_BACKEND_OSS;
#else
    else if (str == "win32") params.sndBackend = SND_BACKEND_WIN32;
#endif
    else params.sndBackend = default_snd_backend;

    params.sdlBufferSize = env.GetInt("sound", "sdlbuffersize", 4);

#ifdef __unix__
    params.soundParam = env.GetInt("sound", "ossfragnum", 8);
#endif

#ifdef _WIN32
    params.soundParam = config.GetInt("sound", "wqsize", 4);
#endif

    // cputrace
    params.cpuTraceEnabled = env.GetBool("cputrace", "enable", false);
    str = env.GetString("cputrace", "format", "[PC]> [M1]:[M2] dT=[DT] AF=[AF] BC=[BC] DE=[DE] "
      "HL=[HL] IX=[IX] IY=[IY] SP=[SP] I=[I] R=[R] AF'=[AF'] BC'=[BC'] DE'=[DE'] HL'=[HL'] "
      "IFF1=[IFF1] IFF2=[IFF2] IM=[IM] INTR=[INTR]");
    strcpy(params.cpuTraceFormat, str.c_str());
    str = env.GetString("cputrace", "filename", "cputrace.log");
    strcpy(params.cpuTraceFileName, str.c_str());

    int spec = SDL_INIT_VIDEO;
    if (params.sound && params.sndBackend == SND_BACKEND_SDL) spec |= SDL_INIT_AUDIO;
    if (SDL_Init(spec) < 0) StrikeError("Unable to init SDL: %s\n", SDL_GetError());

#ifdef _WIN32
    windows_init();
    atexit(windows_cleanup);
#else
    atexit(SDL_Quit);
#endif

    videoSpec = SDL_SWSURFACE;
    if (params.fullscreen) videoSpec |= SDL_FULLSCREEN;
    if (params.useFlipSurface) videoSpec |= SDL_DOUBLEBUF;

    int actualWidth = WIDTH;
    int actualHeight = HEIGHT;

    if (params.scale2x)
    {
      actualWidth *= 2;
      actualHeight *= 2;
    }

    realScreen = SDL_SetVideoMode(actualWidth, actualHeight, 32, videoSpec);

    if (realScreen == nullptr)
      StrikeError("Unable to set requested video mode: %s\n", SDL_GetError());
    REAL_PITCH = realScreen->pitch / 4;

    if (params.scale2x)
    {
      SDL_PixelFormat *fmt = realScreen->format;

      screen = SDL_CreateRGBSurface(SDL_SWSURFACE, WIDTH, HEIGHT, fmt->BitsPerPixel, fmt->Rmask,
                                    fmt->Gmask, fmt->Bmask, 0);
      if (screen == nullptr)
        StrikeError("Unable to create screen surface: %s\n", SDL_GetError());

      PITCH = screen->pitch / 4;
    }
    else
    {
      screen = realScreen;
      PITCH = REAL_PITCH;
    }

    SDL_WM_SetCaption("ZEmu", "ZEmu");
    SDL_ShowCursor(SDL_DISABLE);

#ifdef _WIN32
    strcpy(tempFolderName, "./_temp");
#else
    strcpy(tempFolderName, "/tmp/zemu-XXXXXX");
    if (!mkdtemp(tempFolderName)) DEBUG_MESSAGE("mkdtemp failed");
#endif

    atexit(FreeAll);

    if (params.cpuTraceEnabled)
    {
      DoCpuStep = TraceCpuStep;
      DoCpuInt = TraceCpuInt;
      CpuTrace_Init();
    }

    InitAll();
    ResetSequence();

    if (env.GetBool("core", "trdos_at_start", false))
    {
      dev_mman.OnOutputByte(0x7FFD, 0x10);
      dev_trdos.Enable();
    }

    if (argc != 1) ParseCmdLine(argc, argv);
    InitAudio();

    // [boo_boo]
    C_JoystickManager::Instance()->Init();
    // [/boo_boo]

    Process();

    if (params.cpuTraceEnabled) CpuTrace_Close();
  }
  catch (C_E &e)
  {
    StrikeError("Error %d: %s", e.exc, e.Descr());
  }

  return 0;
}
