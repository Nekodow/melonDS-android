/*
    Project PM online bridge for melonDS Android.

    Ported from ComicartOlie/melonDS-Project-PM (branch platinum-mp),
    src/frontend/qt_sdl/EmuThread.cpp: the mpnet transport, the PMRELAY1 relay
    client and BridgePump. Only the online relay mode is carried over; the
    Windows-only bits (winsock, firewall helper) and the debug tooling
    (autopilot, watchdog, hang dumps) are left out. Wire behaviour is unchanged
    so an Android player can share a room with the Windows builds.

    melonDS is free software, licensed under the GNU GPL v3 or later.
*/

#include "PMBridge.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <android/log.h>

using namespace melonDS;

#define BR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "PMBridge", __VA_ARGS__)

namespace PMBridge
{
namespace
{

typedef int SOCKET;
const SOCKET INVALID_SOCKET = -1;

u32 nowMs()
{
    using namespace std::chrono;
    return (u32)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool wouldBlock()
{
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}

void closeSocket(SOCKET s)
{
    if (s != INVALID_SOCKET) ::close(s);
}

void setNonBlock(SOCKET s)
{
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int nd = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
}

// Non-blocking connect status: 1 connected, 0 still connecting, -1 failed.
// POSIX reports a failed connect as "writable" with SO_ERROR set.
int connectStatus(SOCKET s)
{
    fd_set wr; FD_ZERO(&wr); FD_SET(s, &wr);
    timeval tv = {0, 0};
    int r = select(s + 1, nullptr, &wr, nullptr, &tv);
    if (r < 0) return -1;
    if (r == 0 || !FD_ISSET(s, &wr)) return 0;
    int err = 0; socklen_t len = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return -1;
    return 1;
}

int sendBytes(SOCKET s, const void* data, size_t len)
{
    return (int)::send(s, data, len, MSG_NOSIGNAL);
}

void setStr(char* dst, size_t cap, const char* src)
{
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}
template<size_t N> void setStr(char (&dst)[N], const char* src) { setStr(dst, N, src); }

u32 rd32(NDS* nds, u32 addr) { return *(u32*)&nds->MainRAM[addr & nds->MainRAMMask]; }
u16 rd16(NDS* nds, u32 addr) { return *(u16*)&nds->MainRAM[addr & nds->MainRAMMask]; }
u8 rd8(NDS* nds, u32 addr) { return nds->MainRAM[addr & nds->MainRAMMask]; }
void wr32(NDS* nds, u32 addr, u32 v) { *(u32*)&nds->MainRAM[addr & nds->MainRAMMask] = v; }
void wr8(NDS* nds, u32 addr, u8 v) { nds->MainRAM[addr & nds->MainRAMMask] = v; }
u8* ptr(NDS* nds, u32 addr) { return &nds->MainRAM[addr & nds->MainRAMMask]; }

const u32 MAXFRAME = 8192;

// Bridge wire protocol version, sent to the relay, which refuses to splice a
// joiner whose version differs from the host's. Must match the PC fork.
const u8 WIREVER = 2;   // 2 = 8-player rooms (roles 1..8; ROM protocol v18)

const u32 RELAY_RETRY_MS = 6000;    // protocol floor is 5 s
const u32 RELAY_HS_MS = 20000;      // no reply to a hello: redial
const u32 RELAY_MAXLINE = 200;

enum { RL_DEAD = 0, RL_CONNECTING, RL_HANDSHAKE, RL_UP };

// One outbound relay connection during (and just after) its handshake.
struct RelayLink
{
    SOCKET s = INVALID_SOCKET;
    int st = RL_DEAD;
    std::string out;                // handshake bytes still to send
    std::vector<u8> in;             // read but not yet consumed as a line
    u32 startMs = 0;
    bool closed = false;            // relay hung up; buffered lines still count

    void close()
    {
        closeSocket(s);
        s = INVALID_SOCKET; st = RL_DEAD; out.clear(); in.clear(); closed = false;
    }

    bool hasLine() const
    {
        return std::find(in.begin(), in.end(), (u8)'\n') != in.end();
    }

    // Hands the live socket to a peer slot without closing it.
    SOCKET release() { SOCKET r = s; s = INVALID_SOCKET; close(); return r; }

    bool begin(const sockaddr_in& addr, const char* hello, u32 now)
    {
        close();
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return false;
        setNonBlock(s);
        out = hello;
        startMs = now;
        int r = ::connect(s, (const sockaddr*)&addr, sizeof(addr));
        if (r == 0) st = RL_HANDSHAKE;
        else if (wouldBlock()) st = RL_CONNECTING;
        else { close(); return false; }
        return true;
    }

    // false = the link died. Bounded per-frame work, never blocks.
    bool pump()
    {
        if (st == RL_DEAD) return false;
        if (st == RL_CONNECTING)
        {
            int c = connectStatus(s);
            if (c < 0) return false;
            if (c == 0) return true;
            st = RL_HANDSHAKE;
        }
        if (!out.empty())
        {
            int r = sendBytes(s, out.data(), out.size());
            if (r > 0) out.erase(0, r);
            else if (r < 0 && !wouldBlock()) return false;
        }
        char tmp[8192];
        for (;;)
        {
            ssize_t r = ::recv(s, tmp, sizeof(tmp), 0);
            if (r > 0)
            {
                in.insert(in.end(), tmp, tmp + r);
                if (in.size() > 256*1024) return false;
            }
            else if (r == 0) { closed = true; break; }
            else
            {
                if (!wouldBlock()) closed = true;
                break;
            }
        }
        // A terminal "ERR <token>" and the close that follows it often land in
        // one segment: only report death once no complete line is left.
        if (closed && !hasLine()) return false;
        return true;
    }

    // Pops one '\n'-terminated line. Bytes past it stay in `in`: on a joiner or
    // accept link they are the first game-stream bytes.
    bool takeLine(std::string& line)
    {
        for (size_t i = 0; i < in.size(); i++)
        {
            if (in[i] != '\n') continue;
            size_t e = i;
            if (e && in[e-1] == '\r') e--;
            line.assign((const char*)in.data(), e);
            in.erase(in.begin(), in.begin() + i + 1);
            return true;
        }
        return false;
    }

    bool overflow() const
    {
        return in.size() > RELAY_MAXLINE && !hasLine();
    }
};

bool relayErrTerminal(const char* tok)
{
    return !strcmp(tok, "NOROOM") || !strcmp(tok, "VERSION")
        || !strcmp(tok, "FULL")   || !strcmp(tok, "BADREQ");
}

const char* relayErrText(const char* tok)
{
    if (!strcmp(tok, "NOROOM"))  return "no room with that code";
    if (!strcmp(tok, "VERSION")) return "the host runs a different version";
    if (!strcmp(tok, "FULL"))    return "the room is full";
    if (!strcmp(tok, "BADREQ"))  return "the relay rejected the request";
    if (!strcmp(tok, "RATE"))    return "relay is rate limiting, retrying";
    if (!strcmp(tok, "TIMEOUT")) return "the host did not answer, retrying";
    if (!strcmp(tok, "CLOSED"))  return "the room closed, retrying";
    return "relay error, retrying";
}

struct Peer
{
    SOCKET s = INVALID_SOCKET;
    bool up = false;
    bool reserved = false;      // online host: an accept connection is in flight
    std::vector<u8> rx, tx;
};

// UI handoff: the UI thread posts a request and polls the status; the
// emulation thread consumes the request in its pump.
struct UiRequest
{
    int want = 0;               // 0 none, 1 host, 2 join, 3 stop
    sockaddr_in addr = {};
    char disp[96] = "";
    char name[40] = "";
    char code[8] = "";
};

std::mutex gUiMx;
UiRequest gUiReq;
Status gUiStatus;

struct Net
{
    int mode = 0;                       // 0 off, 1 host, 2 join
    Peer peers[7];                      // host: slot i = role 2+i; join: peers[0] = host
    bool freshPeer = false;             // link just came up: resend on-change channels
    int assignedRole = 0;               // join: role handed out by the host

    int online = 0;                     // 1 host via relay, 2 join via relay
    sockaddr_in relayAddr = {};
    char relaySrv[96] = "";
    char roomCode[8] = "";
    char lobbyName[40] = "Android";
    char onlineMsg[128] = "";
    bool onlineFatal = false;
    u32 onlineRetryMs = 0;
    u32 onlineLogMs = 0;
    u32 ctlSeenMs = 0;
    RelayLink ctl;                      // host control connection
    RelayLink jlink;                    // joiner connection (becomes peers[0])
    struct AcceptLink { RelayLink l; int slot = -1; int ticket = 0; };
    AcceptLink acc[7];

    // lobby control frames (shared with the DeSmuME and BizHawk forks):
    //   name: [0xFE][role][ping u16 LE][len u8][name]   ping: [0xFD][role][tick u32 LE]
    //   pong: [0xFC][role][tick u32 LE]
    char myName[24] = "Android";
    char rname[9][24] = {{0}};
    u16 rping[9] = {0};
    u16 myPingMs = 0;
    u32 pingSentAt = 0;
    u32 pongRx = 0;
    u32 lastNameF = 0, lastPingF = 0;
    u32 lobbySeen[9] = {0};

    bool newSession = false;            // a session just started: reset per-session bridge state

    int myRole() const { return (mode == 1) ? 1 : (assignedRole ? assignedRole : 2); }
    bool anyUp() const { for (int i = 0; i < 7; i++) if (peers[i].up) return true; return false; }
    int upCount() const { int n = 0; for (int i = 0; i < 7; i++) if (peers[i].up) n++; return n; }

    void setMsg(const char* m) { setStr(onlineMsg, m); }

    void sendName(int role)
    {
        u8 buf[29];
        int nl = (int)strlen(myName); if (nl > 23) nl = 23;
        buf[0] = 0xFE; buf[1] = (u8)role;
        buf[2] = (u8)(myPingMs & 0xFF); buf[3] = (u8)(myPingMs >> 8);
        buf[4] = (u8)nl;
        memcpy(buf + 5, myName, nl);
        sendAll(buf, 5 + nl);
        if (role >= 1 && role <= 8)
        {
            memcpy(rname[role], myName, nl); rname[role][nl] = 0;
            rping[role] = myPingMs;
        }
    }

    void sendPing(int role)
    {
        if (mode != 2 || !peers[0].up) return;
        pingSentAt = nowMs();
        u8 buf[6] = { 0xFD, (u8)role,
            (u8)(pingSentAt & 0xFF), (u8)((pingSentAt >> 8) & 0xFF),
            (u8)((pingSentAt >> 16) & 0xFF), (u8)((pingSentAt >> 24) & 0xFF) };
        enqueue(0, buf, 6);
    }

    // Handles control frames; must run before the game-bundle path.
    bool handleCtlFrame(int pi, const u8* rx, u32 n)
    {
        if (n == 2 && rx[0] == 0xFF)                    // host-assigned role
        {
            if (assignedRole != rx[1])
                BR_LOG("host assigned role %d", rx[1]);
            assignedRole = rx[1];
            sendName(myRole());
            return true;
        }
        if (n == 6 && rx[0] == 0xFD)                    // ping: echo back as pong
        {
            u8 pong[6]; memcpy(pong, rx, 6); pong[0] = 0xFC;
            enqueue(pi, pong, 6);
            return true;
        }
        if (n == 6 && rx[0] == 0xFC)                    // pong: round trip is our ping
        {
            u32 tick = rx[2] | (rx[3] << 8) | (rx[4] << 16) | ((u32)rx[5] << 24);
            u32 rtt = nowMs() - tick;
            myPingMs = (rtt > 9999) ? 9999 : (u16)rtt;
            pongRx++;
            return true;
        }
        if (n >= 5 && rx[0] == 0xFE)                    // lobby name announce
        {
            int r = rx[1];
            u16 png = (u16)(rx[2] | (rx[3] << 8));
            int nl = rx[4];
            if (r >= 1 && r <= 8 && nl <= 23 && n >= (u32)(5 + nl))
            {
                bool isNew = (lobbySeen[r] == 0) || strncmp(rname[r], (const char*)rx + 5, nl) != 0;
                memcpy(rname[r], rx + 5, nl); rname[r][nl] = 0;
                rping[r] = png;
                lobbySeen[r] = nowMs();
                if (isNew)
                    BR_LOG("lobby name: role %d = \"%s\" (ping %u ms)", r, rname[r], png);
                // host relays names so every client learns the full room
                if (myRole() == 1)
                    for (int pj = 0; pj < 7; pj++)
                        if (pj != pi) enqueue(pj, rx, n);
            }
            return true;
        }
        return false;
    }

    u32 peekFrame(int i, u8& tag) const
    {
        const Peer& p = peers[i];
        if (p.rx.size() < 2) return 0;
        u32 n = (u32)p.rx[0] | ((u32)p.rx[1] << 8);
        if (n == 0 || n > MAXFRAME) return 0;
        if (p.rx.size() < 2 + n) return 0;
        tag = p.rx[2];
        return n;
    }

    // Lobby-level drain so the roster and the role work before the ROM
    // publishes its bridge block. Stops at the first game bundle.
    void drainControl(int pi)
    {
        for (int guard = 0; guard < 32; guard++)
        {
            u8 tag = 0;
            u32 n = peekFrame(pi, tag);
            if (!n || n > 64) return;
            if (tag != 0xFF && tag != 0xFE && tag != 0xFD && tag != 0xFC) return;
            u8 buf[64];
            u32 got = recvFrame(pi, buf, sizeof(buf));
            if (!got) return;
            handleCtlFrame(pi, buf, got);
        }
    }

    void lobbyTick(u32 frame)
    {
        for (int i = 0; i < 7; i++) drainControl(i);
        if (!anyUp()) return;
        if ((mode == 1 || assignedRole) && frame - lastNameF >= 30)
        {
            lastNameF = frame;
            sendName(myRole());
        }
        if (mode == 2 && frame - lastPingF >= 30)
        {
            lastPingF = frame;
            sendPing(myRole());
        }
    }

    void startHostOnline(const sockaddr_in& addr, const char* disp, const char* name)
    {
        mode = 1; online = 1;
        relayAddr = addr;
        setStr(relaySrv, disp);
        if (name && name[0]) setStr(lobbyName, name);
        setStr(myName, lobbyName);
        roomCode[0] = 0; onlineFatal = false; onlineRetryMs = 0;
        setMsg("connecting to the relay...");
        BR_LOG("online host: relay %s, name \"%s\", wire ver %d", relaySrv, lobbyName, (int)WIREVER);
    }

    void startJoinOnline(const sockaddr_in& addr, const char* disp, const char* code, const char* name)
    {
        mode = 2; online = 2;
        relayAddr = addr;
        setStr(relaySrv, disp);
        if (name && name[0]) setStr(lobbyName, name);
        setStr(myName, lobbyName);
        onlineFatal = false; onlineRetryMs = 0;
        setStr(roomCode, code);
        for (char* p = roomCode; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        setMsg("connecting...");
        BR_LOG("online join: relay %s, room %s, name \"%s\"", relaySrv, roomCode, lobbyName);
    }

    void dropPeer(int i)
    {
        Peer& p = peers[i];
        closeSocket(p.s);
        p.s = INVALID_SOCKET; p.up = false; p.reserved = false;
        p.rx.clear(); p.tx.clear();
        if (online == 2 && i == 0)
        {
            onlineRetryMs = nowMs() + RELAY_RETRY_MS;
            setMsg("link lost, reconnecting");
            BR_LOG("online join: link lost, redialling room %s", roomCode);
        }
    }

    void applyErr(const char* tok, u32 now, const char* who)
    {
        setMsg(relayErrText(tok));
        if (relayErrTerminal(tok))
        {
            onlineFatal = true;
            BR_LOG("online %s: %s (ERR %s) - stopping", who, onlineMsg, tok);
        }
        else
        {
            onlineRetryMs = now + RELAY_RETRY_MS;
            BR_LOG("online %s: %s (ERR %s)", who, onlineMsg, tok);
        }
    }

    void startAccept(int tid, u32 now)
    {
        int slot = -1, ai = -1;
        for (int i = 0; i < 7; i++)
            if (!peers[i].up && !peers[i].reserved) { slot = i; break; }
        for (int i = 0; i < 7; i++)
            if (acc[i].l.st == RL_DEAD) { ai = i; break; }
        if (slot < 0 || ai < 0)
        {
            // no free slot: leave the ticket unclaimed, the relay expires it
            BR_LOG("online host: ticket %d ignored, no free slot", tid);
            return;
        }
        char hello[128];
        snprintf(hello, sizeof(hello), "PMRELAY1 ACCEPT %s %d\n", roomCode, tid);
        if (!acc[ai].l.begin(relayAddr, hello, now)) return;
        acc[ai].slot = slot; acc[ai].ticket = tid;
        peers[slot].reserved = true;
        BR_LOG("online host: ticket %d -> accept connection (slot %d, role %d)", tid, slot, 2 + slot);
    }

    // Spliced: hand the socket to the peer slot, leftover handshake bytes included.
    void installLink(RelayLink& l, int i)
    {
        Peer& p = peers[i];
        p.rx.assign(l.in.begin(), l.in.end());
        p.tx.clear();
        p.s = l.release();
        p.up = true; p.reserved = false;
        freshPeer = true;
    }

    void acceptTick(int i, u32 now)
    {
        AcceptLink& a = acc[i];
        if (a.l.st == RL_DEAD) return;
        bool bad = !a.l.pump() || a.l.overflow();
        if (!bad && a.l.st != RL_UP && now - a.l.startMs > RELAY_HS_MS)
        {
            BR_LOG("online host: ticket %d accept timed out", a.ticket);
            bad = true;
        }
        std::string line;
        if (!bad && a.l.takeLine(line))
        {
            char v[32] = {0}, t[64] = {0};
            sscanf(line.c_str(), "%31s %63s", v, t);
            if (!strcmp(v, "OK"))
            {
                int slot = a.slot;
                installLink(a.l, slot);
                u8 ctlf[4] = { 2, 0, 0xFF, (u8)(2 + slot) };
                peers[slot].tx.insert(peers[slot].tx.end(), ctlf, ctlf + 4);
                flush(slot);
                BR_LOG("peer accepted (online ticket %d) -> role %d", a.ticket, 2 + slot);
                a.slot = -1; a.ticket = 0;
                return;
            }
            BR_LOG("online host: ticket %d rejected (%s %s)", a.ticket, v, t);
            bad = true;
        }
        if (bad)
        {
            if (a.slot >= 0) peers[a.slot].reserved = false;
            a.slot = -1;
            a.l.close();
        }
    }

    void hostOnlineTick(u32 now)
    {
        for (int i = 0; i < 7; i++) acceptTick(i, now);

        if (ctl.st == RL_DEAD)
        {
            if (onlineFatal || now < onlineRetryMs) return;
            char hello[300];
            snprintf(hello, sizeof(hello), "PMRELAY1 HOST %d %s\n", (int)WIREVER, lobbyName);
            if (!ctl.begin(relayAddr, hello, now))
            {
                onlineRetryMs = now + RELAY_RETRY_MS;
                setMsg("relay unreachable, retrying");
                return;
            }
            roomCode[0] = 0;
            ctlSeenMs = now;
            setMsg("connecting to the relay...");
            return;
        }

        bool bad = !ctl.pump() || ctl.overflow();
        bool said = false;
        if (!bad && ctl.st != RL_UP && now - ctl.startMs > RELAY_HS_MS) bad = true;
        // the relay pings every 30 s; long silence means the room is gone
        if (!bad && ctl.st == RL_UP && now - ctlSeenMs > 120000) bad = true;
        std::string line;
        while (!bad && ctl.takeLine(line))
        {
            ctlSeenMs = now;
            char v[32] = {0}, t[64] = {0};
            sscanf(line.c_str(), "%31s %63s", v, t);
            if (!strcmp(v, "OK") && t[0])
            {
                ctl.st = RL_UP;
                setStr(roomCode, t);
                char m[128];
                snprintf(m, sizeof(m), "room code %s on %s", roomCode, relaySrv);
                setMsg(m);
                BR_LOG("online room code %s on %s", roomCode, relaySrv);
            }
            else if (!strcmp(v, "JOIN")) startAccept(atoi(t), now);
            else if (!strcmp(v, "PING")) ctl.out += "PONG\n";
            else if (!strcmp(v, "ERR"))
            {
                applyErr(t, now, "host");
                bad = true; said = true;
            }
        }

        if (bad)
        {
            ctl.close();
            roomCode[0] = 0;
            if (!onlineFatal && !said)
            {
                onlineRetryMs = now + RELAY_RETRY_MS;
                setMsg("relay link lost, reconnecting");
                BR_LOG("online host: relay link lost, reconnecting");
            }
        }
    }

    void joinOnlineTick(u32 now)
    {
        if (peers[0].up) return;

        if (jlink.st == RL_DEAD)
        {
            if (onlineFatal || now < onlineRetryMs) return;
            char hello[300];
            snprintf(hello, sizeof(hello), "PMRELAY1 JOIN %s %d %s\n", roomCode, (int)WIREVER, lobbyName);
            if (!jlink.begin(relayAddr, hello, now))
            {
                onlineRetryMs = now + RELAY_RETRY_MS;
                setMsg("relay unreachable, retrying");
                return;
            }
            char m[128]; snprintf(m, sizeof(m), "room %s (connecting...)", roomCode);
            setMsg(m);
            BR_LOG("online join: dialling room %s via %s", roomCode, relaySrv);
            return;
        }

        bool bad = !jlink.pump() || jlink.overflow();
        bool said = false;
        if (!bad && now - jlink.startMs > RELAY_HS_MS)
        {
            setMsg("the relay did not answer, retrying");
            bad = true;
        }
        std::string line;
        if (!bad && jlink.takeLine(line))
        {
            char v[32] = {0}, t[64] = {0};
            sscanf(line.c_str(), "%31s %63s", v, t);
            if (!strcmp(v, "OK"))
            {
                installLink(jlink, 0);
                char m[128]; snprintf(m, sizeof(m), "room %s (connected)", roomCode);
                setMsg(m);
                BR_LOG("online join: spliced into room %s", roomCode);
                return;
            }
            if (!strcmp(v, "ERR")) { applyErr(t, now, "join"); said = true; }
            bad = true;
        }
        if (bad)
        {
            jlink.close();
            if (!onlineFatal && !said) onlineRetryMs = now + RELAY_RETRY_MS;
        }
    }

    void onlineTick()
    {
        u32 now = nowMs();
        if (online == 1) hostOnlineTick(now);
        else if (online == 2) joinOnlineTick(now);

        if (now - onlineLogMs >= 10000)
        {
            onlineLogMs = now;
            BR_LOG("online %s: room %s, state \"%s\", links=%d, role=%d, pongs=%u",
                (online == 1) ? "host" : "join", roomCode[0] ? roomCode : "-",
                onlineMsg, upCount(), myRole(), pongRx);
        }
    }

    void publishStatus()
    {
        std::lock_guard<std::mutex> lk(gUiMx);
        gUiStatus.mode = online;
        gUiStatus.peers = upCount();
        gUiStatus.pending = (gUiReq.want != 0);
        gUiStatus.code = roomCode;
        gUiStatus.server = relaySrv;
        gUiStatus.text = onlineMsg;
        gUiStatus.myRole = online ? myRole() : 0;
        for (int r = 1; r <= 8; r++)
        {
            gUiStatus.roster[r] = online ? rname[r] : "";
            gUiStatus.rosterPing[r] = rping[r];
        }
    }

    // Picks up a start/stop request posted by the UI thread.
    void pollRequest()
    {
        UiRequest r;
        {
            std::lock_guard<std::mutex> lk(gUiMx);
            if (!gUiReq.want) return;
            r = gUiReq;
            gUiReq.want = 0;
        }
        shutdown();
        newSession = true;
        if (r.want == 1) startHostOnline(r.addr, r.disp, r.name);
        else if (r.want == 2) startJoinOnline(r.addr, r.disp, r.code, r.name);
        else
        {
            setMsg("");
            BR_LOG("online: session stopped");
        }
        publishStatus();
    }

    void tick(u32 frame)
    {
        if (mode == 0) return;
        if (online) onlineTick();
        for (int i = 0; i < 7; i++) { flush(i); pumpRecv(i); }
        lobbyTick(frame);
        publishStatus();
    }

    void enqueue(int i, const u8* p, u32 n)
    {
        Peer& pr = peers[i];
        if (!pr.up || n == 0 || n > MAXFRAME) return;
        if (pr.tx.size() > 512*1024) return;
        u8 hdr[2] = { (u8)(n & 0xFF), (u8)(n >> 8) };
        pr.tx.insert(pr.tx.end(), hdr, hdr + 2);
        pr.tx.insert(pr.tx.end(), p, p + n);
        flush(i);
    }
    void sendAll(const u8* p, u32 n) { for (int i = 0; i < 7; i++) enqueue(i, p, n); }

    void flush(int i)
    {
        Peer& p = peers[i];
        if (!p.up || p.tx.empty()) return;
        int r = sendBytes(p.s, p.tx.data(), p.tx.size());
        if (r > 0) p.tx.erase(p.tx.begin(), p.tx.begin() + r);
        else if (r < 0 && !wouldBlock()) dropPeer(i);
    }

    void pumpRecv(int i)
    {
        Peer& p = peers[i];
        if (!p.up) return;
        char tmp[16384];
        for (;;)
        {
            ssize_t r = ::recv(p.s, tmp, sizeof(tmp), 0);
            if (r > 0)
            {
                p.rx.insert(p.rx.end(), tmp, tmp + r);
                if (p.rx.size() > 1024*1024) { dropPeer(i); return; }
            }
            else if (r == 0) { dropPeer(i); return; }
            else { if (!wouldBlock()) dropPeer(i); return; }
        }
    }

    u32 recvFrame(int i, u8* out, u32 outMax)
    {
        Peer& p = peers[i];
        if (p.rx.size() < 2) return 0;
        u32 n = (u32)p.rx[0] | ((u32)p.rx[1] << 8);
        if (n == 0 || n > MAXFRAME) { dropPeer(i); return 0; }
        if (p.rx.size() < 2 + n) return 0;
        u32 c = (n <= outMax) ? n : outMax;
        memcpy(out, p.rx.data() + 2, c);
        p.rx.erase(p.rx.begin(), p.rx.begin() + 2 + n);
        return c;
    }

    void shutdown()
    {
        online = 0;                 // before dropPeer: no redial
        for (int i = 0; i < 7; i++) dropPeer(i);
        ctl.close(); jlink.close();
        for (int i = 0; i < 7; i++) { acc[i].l.close(); acc[i].slot = -1; acc[i].ticket = 0; }
        roomCode[0] = 0; onlineFatal = false; onlineRetryMs = 0;
        assignedRole = 0;
        memset(rname, 0, sizeof(rname));
        memset(rping, 0, sizeof(rping));
        memset(lobbySeen, 0, sizeof(lobbySeen));
        myPingMs = 0; pongRx = 0;
        mode = 0;
    }
};

Net gNet;

// What the bridge knows about the loaded ROM's mailboxes.
struct BridgeSt
{
    u32 frame = 0;
    u32 disc = 0, ctl = 0;
    u32 exportBlk = 0, importBlk = 0, partyExp = 0, partyImp = 0;
    u32 pktExp = 0, pktImp = 0, owExp = 0, owImp = 0;
    u32 blkN = 0, partyN = 0;
    u32 blkSize = 0, partySize = 0, pktSize = 0;
    u8 beat = 0;
    std::vector<u8> lastParty, lastPkt;
    u32 roleSeenAt[9] = {0};
    u8 gameEver[9] = {0};           // ever game-active latch (3+ player strictness)
    u8 lastPairRole = 0xFF;
    u8 lastOwnInBattle = 0;

    // Game frames received while Pump was not running (pause, long stall):
    // the newest one per (role, channel), applied when Pump resumes. Every
    // channel is a snapshot, so the newest frame carries the full state, but
    // party/pkt are only sent on change and must never be lost.
    std::vector<u8> deferred[9][4];
    int deferredPeer[9][4] = {};

    // diagnostics, shown in the session dialog
    u32 txBundles = 0, txParty = 0, txPkt = 0;
    u32 rxBundles = 0, rxParty = 0, rxPkt = 0;
    u32 rxRejectedLegacy = 0, rxBadSize = 0;
    u8 lastWanted = 0;
    bool lastStrict = false;

    u8 FreshPeerMask(int myRole) const
    {
        u8 m = 0;
        for (int r = 1; r <= 8; r++)
            if (r != myRole && roleSeenAt[r] != 0 && frame - roleSeenAt[r] <= 180)
                m |= (u8)(1 << (r - 1));
        return m;
    }
};

BridgeSt gBr;

// Roles are per session: forget who was seen in a previous one, or a player
// seen under a second role would latch the 3+ player strict routing.
void resetSessionState()
{
    for (int r = 0; r < 9; r++)
    {
        gBr.roleSeenAt[r] = 0;
        gBr.gameEver[r] = 0;
        for (int t = 0; t < 4; t++) gBr.deferred[r][t].clear();
    }
    gBr.lastParty.clear();
    gBr.lastPkt.clear();
    gBr.lastPairRole = 0xFF;
    gBr.txBundles = gBr.txParty = gBr.txPkt = 0;
    gBr.rxBundles = gBr.rxParty = gBr.rxPkt = 0;
    gBr.rxRejectedLegacy = gBr.rxBadSize = 0;
}

void checkNewSession()
{
    if (!gNet.newSession) return;
    gNet.newSession = false;
    resetSessionState();
}

// Splits a game frame: false if it is not a valid peer bundle.
bool parseGameFrame(const u8* rx, u32 n, int myRole, int& tag, int& r)
{
    if (n < 4) return false;
    tag = rx[0]; r = rx[1];
    u32 sz = (u32)(rx[2] | (rx[3] << 8));
    if (r < 1 || r > 8 || r == myRole) return false;
    if (tag < 1 || tag > 3) return false;
    return n >= 4 + sz;
}

void hostForward(int pi, const u8* rx, u32 n)
{
    for (int pj = 0; pj < 7; pj++)
        if (pj != pi) gNet.enqueue(pj, rx, n);
}

// Serializes Pump (emulation thread) against Shutdown and the keepalive.
std::mutex gPumpMx;

std::atomic<u32> gLastPumpMs{0};
std::atomic<bool> gKeepaliveStarted{false};

// While the game is paused (pause menu, app in background) Pump stops running.
// Keep the links alive meanwhile: answer the relay's pings, keep the lobby
// roster fresh, relay bundles between a host's clients, and keep the newest
// game frame per role and channel for Pump to apply on resume. Short stalls
// (shader compiles on a battle transition) stay below the threshold.
void keepaliveLoop()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (nowMs() - gLastPumpMs.load() < 3000) continue;

        std::lock_guard<std::mutex> lk(gPumpMx);
        gNet.pollRequest();
        checkNewSession();
        if (gNet.mode == 0) continue;

        gBr.frame += 15;    // keeps the lobby announce/ping cadence (~2/s)
        gNet.tick(gBr.frame);

        u8 rx[2100];
        u32 n;
        int myRole = gNet.myRole();
        for (int pi = 0; pi < 7; pi++)
        while ((n = gNet.recvFrame(pi, rx, sizeof(rx))) > 0)
        {
            if (gNet.handleCtlFrame(pi, rx, n)) continue;
            int tag, r;
            if (!parseGameFrame(rx, n, myRole, tag, r)) continue;
            if (myRole == 1) hostForward(pi, rx, n);
            gBr.deferred[r][tag].assign(rx, rx + n);
            gBr.deferredPeer[r][tag] = pi;
        }
    }
}

void ensureKeepalive()
{
    if (!gKeepaliveStarted.exchange(true))
        std::thread(keepaliveLoop).detach();
}

// Applies one peer bundle to the ROM's import mailboxes.
void applyGameFrame(NDS* nds, u8* rx, u32 n, int myRole)
{
    int tag = rx[0], r = rx[1];
    u32 sz = (u32)(rx[2] | (rx[3] << 8));

    // peer newly game-active: resend on-change channels
    bool wasFresh = gBr.roleSeenAt[r] != 0 && gBr.frame - gBr.roleSeenAt[r] <= 180;
    if (!wasFresh)
    {
        gBr.lastParty.clear();
        gBr.lastPkt.clear();
    }
    gBr.roleSeenAt[r] = gBr.frame;
    gBr.gameEver[r] = 1;

    // Legacy-channel pair routing (3+ players): the single pairwise import
    // block / party buffer only takes the chosen partner's data (ROM publishes
    // intent at OW export +0x18). Per-role arrays always update.
    u8 pairRole = rd8(nds, gBr.owExp + 0x18);
    int gamePeers = 0;
    for (int gr = 1; gr <= 8; gr++)
        if (gr != myRole && gBr.gameEver[gr]) gamePeers++;
    bool strict = (gamePeers >= 2);
    bool legacyOpen = (pairRole == 0 && !strict) || r == (int)pairRole;
    gBr.lastStrict = strict;

    if (tag == 1 && sz == gBr.blkSize && n >= 4 + sz + 48)
    {
        gBr.rxBundles++;
        rx[4 + 0x12] = (u8)r;   // stamp playerRole
        if (legacyOpen)
            memcpy(ptr(nds, gBr.importBlk), rx + 4, sz);
        else
            gBr.rxRejectedLegacy++;
        if (gBr.blkN) memcpy(ptr(nds, gBr.blkN + (r-1)*sz), rx + 4, sz);
        memcpy(ptr(nds, gBr.owImp + (r-1)*48), rx + 4 + sz, 48);
    }
    else if (tag == 2 && sz == gBr.partySize)
    {
        gBr.rxParty++;
        if (legacyOpen)
            memcpy(ptr(nds, gBr.partyImp), rx + 4, sz);
        if (gBr.partyN) memcpy(ptr(nds, gBr.partyN + (r-1)*sz), rx + 4, sz);
    }
    else if (tag == 3 && sz == gBr.pktSize)
    {
        gBr.rxPkt++;
        memcpy(ptr(nds, gBr.pktImp + (r-1)*sz), rx + 4, sz);
    }
    else
    {
        // block sizes differ from ours: the peer runs another ROM version
        gBr.rxBadSize++;
    }
}

void publishDebug(NDS* nds, int myRole)
{
    char d[256];
    snprintf(d, sizeof(d),
        "role %d, wanted %u, pair %u%s\n"
        "sent: %u blk / %u party / %u pkt\n"
        "recv: %u blk / %u party / %u pkt\n"
        "legacy refused %u, size mismatch %u\n"
        "sizes blk %u party %u pkt %u, peers 0x%02X",
        myRole, gBr.lastWanted, gBr.owExp ? rd8(nds, gBr.owExp + 0x18) : 0, gBr.lastStrict ? " (strict)" : "",
        gBr.txBundles, gBr.txParty, gBr.txPkt,
        gBr.rxBundles, gBr.rxParty, gBr.rxPkt,
        gBr.rxRejectedLegacy, gBr.rxBadSize,
        gBr.blkSize, gBr.partySize, gBr.pktSize, gBr.FreshPeerMask(myRole));
    std::lock_guard<std::mutex> lk(gUiMx);
    gUiStatus.debug = d;
}

void postRequest(int want, const sockaddr_in* addr, const char* disp, const char* code, const char* name)
{
    std::lock_guard<std::mutex> lk(gUiMx);
    gUiReq.want = want;
    if (addr) gUiReq.addr = *addr;
    setStr(gUiReq.disp, disp);
    setStr(gUiReq.name, name);
    setStr(gUiReq.code, code);
    gUiStatus.pending = true;
    gUiStatus.mode = (want == 3) ? 0 : want;
    gUiStatus.server = disp ? disp : "";
    gUiStatus.code = (want == 2 && code) ? code : "";
    gUiStatus.text = (want == 3) ? "" : "starting (waiting for emulation)";
}

// Blocking DNS: "host" or "host:port".
bool resolve(const char* server, sockaddr_in* out, char* disp, size_t dispCap)
{
    if (!server) return false;
    while (*server == ' ') server++;
    if (!*server) return false;

    char host[128];
    setStr(host, server);
    for (int i = (int)strlen(host) - 1; i >= 0 && host[i] == ' '; i--) host[i] = 0;

    int port = DefaultPort;
    char* colon = strrchr(host, ':');
    if (colon && colon[1])
    {
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536) { port = p; *colon = 0; }
    }
    if (!host[0]) return false;

    addrinfo hints = {};
    hints.ai_family = AF_INET;              // the bridge speaks IPv4
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    addrinfo* res = nullptr;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return false;
    memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);

    snprintf(disp, dispCap, "%s:%d", host, port);
    return true;
}

}

void ResetRomState()
{
    std::lock_guard<std::mutex> lk(gPumpMx);
    gBr = BridgeSt();
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(gPumpMx);
    {
        std::lock_guard<std::mutex> uk(gUiMx);
        gUiReq.want = 0;
    }
    gNet.shutdown();
    gNet.setMsg("");
    gNet.publishStatus();
    gBr = BridgeSt();
}

bool Host(const char* relay, const char* name)
{
    sockaddr_in addr = {};
    char disp[96];
    if (!resolve(relay, &addr, disp, sizeof(disp))) return false;
    postRequest(1, &addr, disp, "", name);
    ensureKeepalive();
    return true;
}

bool Join(const char* relay, const char* code, const char* name)
{
    sockaddr_in addr = {};
    char disp[96];
    if (!resolve(relay, &addr, disp, sizeof(disp))) return false;
    postRequest(2, &addr, disp, code, name);
    ensureKeepalive();
    return true;
}

void Stop()
{
    postRequest(3, nullptr, "", "", "");
}

Status GetStatus()
{
    std::lock_guard<std::mutex> lk(gUiMx);
    return gUiStatus;
}

void Pump(NDS* nds)
{
    std::lock_guard<std::mutex> lk(gPumpMx);
    gLastPumpMs = nowMs();
    gBr.frame++;

    gNet.pollRequest();
    checkNewSession();
    if (gNet.mode == 0) return;
    gNet.tick(gBr.frame);

    // A peer link just came up: resend the on-change channels.
    if (gNet.freshPeer)
    {
        gNet.freshPeer = false;
        gBr.lastParty.clear();
        gBr.lastPkt.clear();
    }

    if (!gBr.disc)
    {
        if ((gBr.frame % 60) != 0) return;
        for (u32 off = 0; off < 0x400000 - 16; off += 4)
        {
            if (*(u32*)&nds->MainRAM[off] == 0xCAFE1234
                && *(u32*)&nds->MainRAM[off+4] == 0x5678CAFE)
            {
                gBr.disc = 0x02000000 + off;
                break;
            }
        }
        if (!gBr.disc)
        {
            std::lock_guard<std::mutex> lk(gUiMx);
            gUiStatus.debug = "ROM bridge not found (not Project PM, or still booting)";
            return;
        }

        gBr.exportBlk = rd32(nds, gBr.disc + 2*4);
        gBr.importBlk = rd32(nds, gBr.disc + 3*4);
        gBr.partyExp  = rd32(nds, gBr.disc + 4*4);
        gBr.partyImp  = rd32(nds, gBr.disc + 5*4);
        gBr.pktExp    = rd32(nds, gBr.disc + 12*4);
        gBr.pktImp    = rd32(nds, gBr.disc + 13*4);
        gBr.owExp     = rd32(nds, gBr.disc + 14*4);
        gBr.owImp     = rd32(nds, gBr.disc + 15*4);
        gBr.blkSize   = rd32(nds, gBr.disc + 20*4) & 0xFFFF;
        gBr.blkN      = rd32(nds, gBr.disc + 25*4);
        gBr.partyN    = rd32(nds, gBr.disc + 26*4);
        gBr.ctl       = rd32(nds, gBr.disc + 31*4);
        BR_LOG("ROM discovery block at %08X, bridge ctl %08X", gBr.disc, gBr.ctl);
    }
    if (!gBr.ctl || rd32(nds, gBr.ctl) != 0x42524731)     // "BRG1"
    {
        if ((gBr.frame % 30) == 0)
        {
            std::lock_guard<std::mutex> lk(gUiMx);
            gUiStatus.debug = "ROM bridge block not active yet";
        }
        return;
    }

    gBr.partySize = rd16(nds, gBr.ctl + 10);
    gBr.pktSize   = rd16(nds, gBr.ctl + 12);

    wr8(nds, gBr.ctl + 6, ++gBr.beat);     // fork heartbeat

    u8 wanted = rd8(nds, gBr.ctl + 4);
    gBr.lastWanted = wanted;
    bool inGame = (wanted && gBr.blkSize != 0 && gBr.blkSize <= 512
        && gBr.partySize != 0 && gBr.partySize <= 2048
        && gBr.pktSize != 0 && gBr.pktSize <= 2048);

    // No early return when !inGame: inbound frames are always drained and
    // applied; only outbound bundles and the status writes gate on inGame.
    if (!inGame)
    {
        wr8(nds, gBr.ctl + 7, 0);
        wr8(nds, gBr.ctl + 9, 0);
    }

    int myRole = gNet.myRole();
    wr8(nds, gBr.ctl + 8, (u8)myRole);

    if (inGame && gNet.anyUp())
    {
        u8 buf[2100];

        // bundle: [1][role][blkSize u16][block][ow 48] - every frame
        buf[0] = 1; buf[1] = (u8)myRole;
        buf[2] = (u8)(gBr.blkSize & 0xFF); buf[3] = (u8)(gBr.blkSize >> 8);
        memcpy(buf + 4, ptr(nds, gBr.exportBlk), gBr.blkSize);
        memcpy(buf + 4 + gBr.blkSize, ptr(nds, gBr.owExp), 48);
        gNet.sendAll(buf, 4 + gBr.blkSize + 48);
        gBr.txBundles++;

        // party: on content change
        if (gBr.lastParty.size() != gBr.partySize
            || memcmp(gBr.lastParty.data(), ptr(nds, gBr.partyExp), gBr.partySize) != 0)
        {
            gBr.lastParty.assign(ptr(nds, gBr.partyExp), ptr(nds, gBr.partyExp) + gBr.partySize);
            buf[0] = 2; buf[1] = (u8)myRole;
            buf[2] = (u8)(gBr.partySize & 0xFF); buf[3] = (u8)(gBr.partySize >> 8);
            memcpy(buf + 4, gBr.lastParty.data(), gBr.partySize);
            gNet.sendAll(buf, 4 + gBr.partySize);
            gBr.txParty++;
        }

        // pkt channel: on content change
        if (gBr.lastPkt.size() != gBr.pktSize
            || memcmp(gBr.lastPkt.data(), ptr(nds, gBr.pktExp), gBr.pktSize) != 0)
        {
            gBr.lastPkt.assign(ptr(nds, gBr.pktExp), ptr(nds, gBr.pktExp) + gBr.pktSize);
            buf[0] = 3; buf[1] = (u8)myRole;
            buf[2] = (u8)(gBr.pktSize & 0xFF); buf[3] = (u8)(gBr.pktSize >> 8);
            memcpy(buf + 4, gBr.lastPkt.data(), gBr.pktSize);
            gNet.sendAll(buf, 4 + gBr.pktSize);
            gBr.txPkt++;
        }
    }

    // receive: apply what arrived while Pump was not running, then the
    // peers' live mailboxes (per peer link; host relays)
    for (int r = 1; r <= 8; r++)
        for (int t = 1; t <= 3; t++)
        {
            std::vector<u8>& f = gBr.deferred[r][t];
            if (f.empty()) continue;
            applyGameFrame(nds, f.data(), (u32)f.size(), myRole);
            f.clear();
        }
    {
        u8 rx[2100];
        u32 n;
        for (int pi = 0; pi < 7; pi++)
        while ((n = gNet.recvFrame(pi, rx, sizeof(rx))) > 0)
        {
            if (gNet.handleCtlFrame(pi, rx, n)) continue;
            int tag, r;
            if (!parseGameFrame(rx, n, myRole, tag, r)) continue;

            // host relay: forward a client bundle to the other clients
            if (myRole == 1) hostForward(pi, rx, n);

            applyGameFrame(nds, rx, n, myRole);
        }
    }

    // Pair rebind: on a pairRole change to a real role, replay that role's
    // latest cached block/party. A change to 0 is left alone (the ROM flaps it).
    if (gBr.owExp && gBr.importBlk)
    {
        u8 pr = rd8(nds, gBr.owExp + 0x18);
        if (pr != gBr.lastPairRole)
        {
            gBr.lastPairRole = pr;
            if (pr >= 1 && pr <= 8)
            {
                if (gBr.blkN)
                    memcpy(ptr(nds, gBr.importBlk), ptr(nds, gBr.blkN + (pr-1)*gBr.blkSize), gBr.blkSize);
                if (gBr.partyN && gBr.partyImp)
                    memcpy(ptr(nds, gBr.partyImp), ptr(nds, gBr.partyN + (pr-1)*gBr.partySize), gBr.partySize);
            }
        }
    }
    // Own battle over: purge the partner-block ghost.
    if (gBr.exportBlk && gBr.importBlk)
    {
        u8 ib = rd8(nds, gBr.exportBlk + 0x10);
        if (!ib && gBr.lastOwnInBattle)
            wr32(nds, gBr.importBlk, 0);
        gBr.lastOwnInBattle = ib;
    }

    // status + peer mask (a bundle from that role within ~3 s)
    if (inGame)
    {
        u8 mask = gBr.FreshPeerMask(myRole);
        wr8(nds, gBr.ctl + 9, mask);
        wr8(nds, gBr.ctl + 7, mask ? 2 : 1);
    }

    if ((gBr.frame % 30) == 0)
        publishDebug(nds, myRole);
}

}
