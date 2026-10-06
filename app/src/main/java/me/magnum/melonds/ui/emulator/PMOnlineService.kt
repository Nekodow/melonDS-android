package me.magnum.melonds.ui.emulator

import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import me.magnum.melonds.MelonDSApplication
import me.magnum.melonds.R

/**
 * Keeps the process running while a Project PM online session is active. Without it Android freezes
 * the app a few seconds after it goes to the background, the relay link drops, and on reconnect the
 * host hands out a new player role, which breaks the pairing used to start co-op battles.
 */
class PMOnlineService : Service() {
    companion object {
        private const val NOTIFICATION_ID = 7833

        fun start(context: Context) {
            try {
                context.startForegroundService(Intent(context, PMOnlineService::class.java))
            } catch (_: Exception) {
                // Not allowed right now (app in the background): the session still works while visible
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, PMOnlineService::class.java))
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = NotificationCompat.Builder(this, MelonDSApplication.NOTIFICATION_CHANNEL_ID_BACKGROUND_TASKS)
            .setSmallIcon(R.drawable.ic_melon_small)
            .setContentTitle(getString(R.string.pm_online))
            .setContentText(getString(R.string.pm_online_notification))
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()

        ServiceCompat.startForeground(this, NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
