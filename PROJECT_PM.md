# melonDS Android: Project PM online play

This is a modified build of [melonDS Android](https://github.com/rafaelvcaetano/melonDS-android). It adds the **"Host / Join Online Game"** room-code multiplayer from the Windows **melonDS Project PM fork** ([ComicartOlie/melonDS-Project-PM](https://github.com/ComicartOlie/melonDS-Project-PM), branch `platinum-mp`).

Project PM is a co-op multiplayer romhack of Pokémon Platinum. With this build, an Android phone can share an online room with players on the Windows melonDS (or DeSmuME) Project PM builds.

## Download

Grab the APK from this repository's **Releases** page. It installs as **"melonDS Dev"**, next to an existing melonDS.

## How to use it

1. Install the APK and load the Project PM ROM. **Every player must use exactly the same ROM file.**
2. In game, press Back to open the pause menu, then choose **Online play (Project PM)**:
   - **Host online game** gives you a 5-character room code to share. No port forwarding is needed.
   - **Join online game** asks for the host's room code.
   - The relay server field is pre-filled with the community relay (`193.122.236.144:7833`), the same default as the PC fork. All players must use the same relay.
3. Turn on the romhack's own multiplayer in game:
   - Bag → Key Items → **Wireless Play** to see each other.
   - Options → MULTIPLAYER → **CO-OP: ON** for multi battles.
   - The **Poké Assist** key item toggles co-op wild encounters.
4. A banner at the top of the screen shows the room and the player count. The same menu shows the room code (with a copy button), the player list with pings, a diagnostics section, and **Leave game**.

The phone can host and PC players join, or the other way around.

## How it works

The Project PM online mode does **not** use the emulated DS Wi-Fi. The ROM publishes "mailboxes" in emulated RAM: player position, battle and trade state, party data and messages. The emulator's *bridge* copies them to the other players over TCP once per frame.

1. **Relay (`PMRELAY1` protocol).** Both sides dial out to a relay server, which splices the two connections together. The host gets a room code, and joiners connect with it. Nobody needs an open port, and players never see each other's IP address.
2. **Framing.** Frames are length-prefixed and identical to the PC forks: `[u16 len][tag][role][size][payload]`.
   - Tag 1 is the per-frame player block.
   - Tags 2 and 3 are the party and message channels, sent when they change.
   - Tags `0xFC` to `0xFF` are lobby control frames: role assignment, names, ping.
3. **Bridge.** Once per emulated frame, the bridge finds the ROM's discovery block (signature `CAFE1234 5678CAFE`), sends our mailboxes and writes the other players' data into the ROM's import mailboxes.

`PMBridge.cpp` is a port of the bridge in the PC fork's `EmuThread.cpp`, with the same wire protocol and relay client. The Windows-only parts (winsock, firewall helper) and the PC fork's debug autopilot were left out.

### Android-specific additions

- **Keepalive during pauses.** When the game is paused (pause menu, app in the background), a keepalive keeps answering the relay and holds incoming data for when the game resumes. Without it, the host's room code would change and data could be lost.
- **Foreground service.** While a session is active, a foreground service with a notification stops Android from freezing the app. If the app froze, the connection would drop, and a reconnecting player gets a new player number, which breaks pairing.
- **3+ player routing.** Only players still in the lobby count for the "strict" routing, as in the DeSmuME bridge. The PC melonDS fork counts every player it has ever seen, which can wrongly lock out 2-player battle requests.
- **Test tools, inactive by default.** Creating files in the app's internal storage turns them on: `pm_trace` logs bridge state, `pm_wireless_on` activates Wireless Play, and `pm_record` records every frame and every mailbox change to `pm_record.log`.

## Status

**Tested between the Android emulator and the Windows melonDS Project PM fork, through the community relay:**
- Hosting and joining in both directions.
- Seeing each other move.
- Name and ping exchange.
- The connection surviving pauses and the app being in the background.

**Not working yet: co-op wild battles between Android and PC.**
- When one side starts a co-op wild encounter, the other side receives the request. Recordings of both games' memory show it arriving intact, but the game never produces the "join" answer.
- The same scenario works between two Windows melonDS instances.
- The investigation, comparing full recordings of both cases, is still in progress.

Untested: trades, item giving, player-vs-player battles, 3+ players.

## Rebuilding from source

Requirements: JDK 21, Android SDK (compileSdk 37) and NDK 28.0.13004108. Gradle downloads anything that is missing.

```bash
git clone --recurse-submodules -b pm-online https://github.com/Nekodow/melonDS-android
cd melonDS-android
./gradlew assembleGitHubProdDebug
```

The APK is written to `app/build/outputs/apk/gitHubProd/debug/`. All changes are in the commits of the `pm-online` branch, on top of melonDS Android `master` (`c42995ca`).

### Files added or changed

- `app/src/main/cpp/PMBridge.cpp`, `PMBridge.h`: the bridge, relay client and recorder.
- `app/src/main/cpp/PMBridgeJNI.cpp`: the JNI glue.
- `app/src/main/cpp/MelonInstance.cpp`, `MelonDS.cpp`: call the bridge once per frame, reset it on ROM load or reset, and shut it down on exit.
- `app/src/main/java/me/magnum/melonds/PMOnline.kt`: the Kotlin API.
- `app/src/main/java/me/magnum/melonds/ui/emulator/PMOnlineController.kt`: dialogs and the status banner.
- `app/src/main/java/me/magnum/melonds/ui/emulator/PMOnlineService.kt`: the foreground service.
- Pause menu entry, layout, FR/EN strings and manifest.

## Credits

melonDS by the melonDS team, melonDS Android by rafaelvcaetano, and the Project PM romhack and its PC bridge and relay by ComicartOlie. All are licensed under the GPL v3, and so is this modification.
