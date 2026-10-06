package me.magnum.melonds

/**
 * Project PM online play: room-code sessions through a PMRELAY1 relay server, compatible with the
 * "Host/Join Online Game..." options of the melonDS Project PM fork for Windows.
 */
object PMOnline {
    data class Player(val role: Int, val name: String, val pingMs: Int)

    data class Status(
        /** 0 off, 1 hosting, 2 joined */
        val mode: Int,
        val connectedLinks: Int,
        val pending: Boolean,
        val roomCode: String,
        val server: String,
        val message: String,
        val myRole: Int,
        val players: List<Player>,
        /** Bridge counters, for troubleshooting */
        val debug: String,
    ) {
        val isActive get() = mode != 0 || pending
        val isHost get() = mode == 1
    }

    val defaultRelay: String by lazy { getDefaultRelayInternal() }

    /** Resolves the relay (blocking DNS). Call off the main thread. Returns false if the relay can't be resolved. */
    fun host(relay: String, name: String): Boolean = hostInternal(relay, sanitizeName(name))

    /** Resolves the relay (blocking DNS). Call off the main thread. Returns false if the relay can't be resolved. */
    fun join(relay: String, code: String, name: String): Boolean = joinInternal(relay, code.trim().uppercase(), sanitizeName(name))

    external fun stop()

    fun getStatus(): Status {
        val raw = getStatusInternal()
        val players = (1..8).mapNotNull { role ->
            val name = raw[5 + role * 2]
            if (name.isEmpty()) null else Player(role, name, raw[6 + role * 2].toIntOrNull() ?: 0)
        }
        return Status(
            mode = raw[0].toIntOrNull() ?: 0,
            connectedLinks = raw[1].toIntOrNull() ?: 0,
            pending = raw[2] == "1",
            roomCode = raw[3],
            server = raw[4],
            message = raw[5],
            myRole = raw[6].toIntOrNull() ?: 0,
            players = players,
            debug = raw.getOrElse(23) { "" },
        )
    }

    // The relay handshake is a single text line: no line breaks, and the bridge keeps 23 bytes of a name
    private fun sanitizeName(name: String): String {
        return name.replace(Regex("[\\r\\n\\t]"), " ").trim().take(20).ifEmpty { "Android" }
    }

    private external fun hostInternal(relay: String, name: String): Boolean
    private external fun joinInternal(relay: String, code: String, name: String): Boolean
    private external fun getDefaultRelayInternal(): String
    private external fun getStatusInternal(): Array<String>
}
