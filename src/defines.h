#ifndef ZEMU_DEFINES_H
#define ZEMU_DEFINES_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <cstdint>
#include <z80ex.h>
#include "platform.h"

#ifndef Z80EX_ZAME_WRAPPER
  #define Z80EX_CONTEXT_PARAM Z80EX_CONTEXT *cpu,
#else
  #define Z80EX_CONTEXT_PARAM
#endif

#define DEBUG_MESSAGE(msg) printf("%s\n",(msg))

extern char hex[17];

double sqq(double n);
int sgn(int a);
int unhex(char c);
bool ishex(char c);

void AddLog(const char *fmt, ...);
void AddLogN(const char *fmt, ...);
void StrikeError(const char *fmt, ...);
void StrikeMessage(const char *fmt, ...);

char* AllocNstrcpy(const char *str);

#endif // ZEMU_DEFINES_H
