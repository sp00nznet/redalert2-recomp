/* mods.c: a mod's folder laid over the game's (Windows host). */
#pragma once
#include "native32.h"

/* The file calls, answered from the mod's folder first: in every shim table. */
void mods_CreateFileA(void);
void mods_GetFileAttributesA(void);
void mods_CreateDirectoryA(void);
void mods_CopyFileA(void);
void mods_FindFirstFileA(void);
void mods_FindNextFileA(void);
void mods_FindClose(void);
#define MODS_SHIMS \
    { "CreateFileA", mods_CreateFileA }, \
    { "GetFileAttributesA", mods_GetFileAttributesA }, \
    { "CreateDirectoryA", mods_CreateDirectoryA }, \
    { "CopyFileA", mods_CopyFileA }, \
    { "FindFirstFileA", mods_FindFirstFileA }, \
    { "FindNextFileA", mods_FindNextFileA }, \
    { "FindClose", mods_FindClose }

void mods_start(const char* game_dir, const char* requested);   /* --mod NAME ("none"), else ra2.ini's */
const char* mods_active(void);                                  /* "" when none */
int  mods_list(char names[][128], int max);                     /* the mods in mods\<game> */
void mods_restart_with(const char* name);                       /* "" for none; does not return if it starts */
