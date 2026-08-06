#ifndef _BS_OVERLAY_FILE_SYSTEM_H_
#define _BS_OVERLAY_FILE_SYSTEM_H_

#include "common.h"
#include "file_system.h"

// OverlayFileSystem implements GameMaker's two-area sandboxed file system on top of plain stdio.
// It holds two base paths:
// * bundlePath: read-only "File Bundle" area, where Included Files and the data.win live.
// * savePath: read/write "Save Area", the only place writes are allowed.
//
// Read operations check savePath first and fall back to bundlePath.
// Writes always target savePath. delete only acts on savePath (it will not touch a same-named file in the bundle).
//
// https://manual.gamemaker.io/lts/en/Additional_Information/The_File_System.htm
typedef struct {
    FileSystem base;
    char* bundlePath; // includes trailing '/'
    char* savePath; // includes trailing '/'
} OverlayFileSystem;

OverlayFileSystem* OverlayFileSystem_create(const char* bundlePath, const char* savePath);

#ifdef PLATFORM_PSP
// Queue background reads of the save slots + dr.ini into the bs_io RAM cache
// (boot-time warm while the player is on the title screen), so the first
// Continue / save-star open needs no stick reads.
// `legacyFallback` = also look in bundlePath for a save the saves/ migration has not
// moved yet. Pass FALSE once the card is known to be fully migrated: an old copy left
// in the game root would otherwise RESURRECT a slot the player erased.
void OverlayFileSystem_warmSaveCache(OverlayFileSystem* ofs, bool legacyFallback);
#endif
void OverlayFileSystem_destroy(OverlayFileSystem* fs);

#endif /* _BS_OVERLAY_FILE_SYSTEM_H_ */
