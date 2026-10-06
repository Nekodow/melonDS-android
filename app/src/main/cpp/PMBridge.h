#ifndef PMBRIDGE_H
#define PMBRIDGE_H

// Project PM online play (port of the ComicartOlie/melonDS-Project-PM
// "platinum-mp" bridge, online relay mode only).
//
// The bridge syncs the romhack's multiplayer mailboxes in emulated main RAM
// with the other players over a TCP link spliced by a PMRELAY1 relay server.
// The emulated DS radio is never touched. Wire-compatible with the Windows
// melonDS / DeSmuME / BizHawk Project PM forks.

#include <string>
#include "NDS.h"

namespace PMBridge
{

struct Status
{
    int mode = 0;                   // 0 off, 1 hosting online, 2 joined online
    int peers = 0;                  // live game links
    bool pending = false;           // request posted, emulation has not run yet
    std::string code;               // room code (empty until the relay assigns)
    std::string server;
    std::string text;               // connection state / last error
    std::string roster[9];          // display name by role 1..8 (slot 0 unused)
    int rosterPing[9] = {};
    int myRole = 0;
    std::string debug;              // bridge counters, for troubleshooting
};

const int DefaultPort = 7833;
const char* const DefaultRelay = "193.122.236.144:7833";

// Called once per emulated frame from the emulation thread, before RunFrame.
void Pump(melonDS::NDS* nds);

// Forget everything learned about the loaded ROM (new ROM / reset).
void ResetRomState();

// Closes the online session immediately (emulator shutdown).
void Shutdown();

// Thread-safe control surface for the UI. Host/Join resolve the relay (blocking
// DNS): call them off the main thread. They return false if the relay address
// could not be resolved.
bool Host(const char* relay, const char* name);
bool Join(const char* relay, const char* code, const char* name);
void Stop();
Status GetStatus();

}

#endif // PMBRIDGE_H
