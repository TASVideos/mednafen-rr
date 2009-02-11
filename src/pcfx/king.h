#ifndef __PCFX_KING_H
#define __PCFX_KING_H

#include "vdc.h"

void KING_RunFrame(fx_vdc_t **, uint32 *pXBuf, MDFN_Rect *LineWidths, int skip);
void KING_SetPixelFormat(int rshift, int gshift, int bshift);
uint16 FXVCE_Read16(uint32 A);
void FXVCE_Write16(uint32 A, uint16 V);

#ifdef WANT_DEBUGGER
uint32 FXVDCVCE_GetRegister(const std::string &name, std::string *special);
void FXVDCVCE_SetRegister(const std::string &name, uint32 value);
#endif

uint8 KING_Read8(uint32 A);
uint16 KING_Read16(uint32 A);

void KING_Write8(uint32 A, uint8 V);
void KING_Write16(uint32 A, uint16 V);
bool KING_Init(void);
void KING_Close(void);
void KING_Reset(void);

uint16 KING_GetADPCMHalfWord(int ch);

uint8 KING_MemPeek(uint32 A);

uint8 KING_RB_Fetch();

bool KING_ToggleLayer(int which);

int KING_StateAction(StateMem *sm, int load, int data_only);

#ifdef WANT_DEBUGGER
uint32 KING_GetRegister(const std::string &name, std::string *special);
void KING_SetRegister(const std::string &name, uint32 value);
#endif

void KING_SetGraphicsDecode(int line, int which, int w, int h, int xscroll, int yscroll, int pbn);
uint32 *KING_GetGraphicsDecodeBuffer(void);

void KING_NotifyOfBPE(bool read, bool write);

void KING_SetLogFunc(void (*logfunc)(const char *, const char *, ...));

void KING_ResetTS(void);

void KING_DoMagic(void);

#endif
