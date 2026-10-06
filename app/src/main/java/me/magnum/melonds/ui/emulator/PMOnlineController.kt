package me.magnum.melonds.ui.emulator

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.text.InputFilter
import android.text.InputType
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.edit
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.PMOnline
import me.magnum.melonds.R

/**
 * UI for Project PM online play: the host/join/session dialogs reached from the pause menu, and the
 * small status line shown over the game while a session is active.
 *
 * @param onDialogShown called when a dialog opens (the emulator is paused at that point)
 * @param onDialogClosed called when the flow ends; the emulator should resume
 */
class PMOnlineController(
    private val activity: AppCompatActivity,
    private val statusView: TextView,
    private val onDialogShown: () -> Unit,
    private val onDialogClosed: () -> Unit,
) {
    private val prefs = activity.getSharedPreferences("pm_online", Context.MODE_PRIVATE)

    fun startStatusUpdates() {
        activity.lifecycleScope.launch {
            activity.lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (isActive) {
                    updateStatusView(PMOnline.getStatus())
                    delay(1000)
                }
            }
        }
    }

    fun showMenu() {
        val status = PMOnline.getStatus()
        if (status.isActive) {
            showSessionDialog(status)
            return
        }

        val options = arrayOf(activity.getString(R.string.pm_online_host), activity.getString(R.string.pm_online_join))
        var handled = false
        onDialogShown()
        AlertDialog.Builder(activity)
            .setTitle(R.string.pm_online)
            .setItems(options) { _, which ->
                handled = true
                if (which == 0) showHostDialog() else showJoinDialog()
            }
            .setNegativeButton(R.string.cancel, null)
            .setOnDismissListener { if (!handled) onDialogClosed() }
            .show()
    }

    private fun showHostDialog() {
        val form = Form()
        AlertDialog.Builder(activity)
            .setTitle(R.string.pm_online_host)
            .setMessage(R.string.pm_online_host_info)
            .setView(form.root)
            .setPositiveButton(R.string.pm_online_host_action) { _, _ ->
                form.save()
                start(form.relay) { PMOnline.host(form.relay, form.name) }
            }
            .setNegativeButton(R.string.cancel) { _, _ -> onDialogClosed() }
            .setOnCancelListener { onDialogClosed() }
            .show()
    }

    private fun showJoinDialog() {
        val form = Form(withCode = true)
        val dialog = AlertDialog.Builder(activity)
            .setTitle(R.string.pm_online_join)
            .setMessage(R.string.pm_online_join_info)
            .setView(form.root)
            .setPositiveButton(R.string.pm_online_join_action, null)
            .setNegativeButton(R.string.cancel) { _, _ -> onDialogClosed() }
            .setOnCancelListener { onDialogClosed() }
            .show()

        // Validate the code before closing the dialog
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            if (form.code.length != 5) {
                form.codeInput?.error = activity.getString(R.string.pm_online_code_invalid)
                return@setOnClickListener
            }
            form.save()
            dialog.dismiss()
            start(form.relay) { PMOnline.join(form.relay, form.code, form.name) }
        }
    }

    private fun showSessionDialog(status: PMOnline.Status) {
        onDialogShown()
        val builder = AlertDialog.Builder(activity)
            .setTitle(R.string.pm_online)
            .setMessage(describeSession(status))
            .setPositiveButton(R.string.ok) { _, _ -> }
            .setNegativeButton(R.string.pm_online_leave) { _, _ ->
                PMOnline.stop()
                Toast.makeText(activity, R.string.pm_online_left, Toast.LENGTH_SHORT).show()
            }
            .setOnDismissListener { onDialogClosed() }

        if (status.roomCode.isNotEmpty()) {
            builder.setNeutralButton(R.string.pm_online_copy_code) { _, _ ->
                val clipboard = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                clipboard.setPrimaryClip(ClipData.newPlainText("Project PM room code", status.roomCode))
                Toast.makeText(activity, R.string.pm_online_code_copied, Toast.LENGTH_SHORT).show()
            }
        }
        builder.show()
    }

    private fun start(relay: String, request: () -> Boolean) {
        activity.lifecycleScope.launch {
            val started = withContext(Dispatchers.IO) { request() }
            if (!started) {
                Toast.makeText(activity, activity.getString(R.string.pm_online_relay_not_found, relay), Toast.LENGTH_LONG).show()
            }
            // The session itself starts on the emulation thread, so resume the game
            onDialogClosed()
        }
    }

    private fun describeSession(status: PMOnline.Status): String {
        val lines = mutableListOf<String>()
        lines += activity.getString(if (status.isHost) R.string.pm_online_role_host else R.string.pm_online_role_guest)
        if (status.roomCode.isNotEmpty()) {
            lines += activity.getString(R.string.pm_online_room_code, status.roomCode)
        }
        if (status.message.isNotEmpty()) {
            lines += activity.getString(R.string.pm_online_state, status.message)
        }
        lines += activity.getString(R.string.pm_online_relay, status.server)
        if (status.players.isNotEmpty()) {
            lines += ""
            lines += activity.getString(R.string.pm_online_players)
            status.players.forEach {
                val me = if (it.role == status.myRole) activity.getString(R.string.pm_online_me) else ""
                val ping = if (it.role == 1) "" else " · ${it.pingMs} ms"
                lines += "  P${it.role} ${it.name}$me$ping"
            }
        }
        return lines.joinToString("\n")
    }

    private fun updateStatusView(status: PMOnline.Status) {
        if (!status.isActive) {
            statusView.visibility = View.GONE
            return
        }

        val text = when {
            status.roomCode.isNotEmpty() && (status.connectedLinks > 0 || status.isHost) -> {
                // The roster keeps names of players who left, so count live links instead
                val playerCount = if (status.isHost) status.connectedLinks + 1 else status.players.size.coerceAtLeast(2)
                activity.getString(R.string.pm_online_overlay_room, status.roomCode, playerCount)
            }
            else -> activity.getString(R.string.pm_online_overlay_connecting, status.message)
        }
        statusView.text = text
        statusView.visibility = View.VISIBLE
    }

    private inner class Form(withCode: Boolean = false) {
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            val padding = (20 * activity.resources.displayMetrics.density).toInt()
            setPadding(padding, padding / 2, padding, 0)
        }

        private val nameInput = addField(R.string.pm_online_name, prefs.getString(KEY_NAME, null) ?: android.os.Build.MODEL ?: "Android")
        val codeInput: EditText? = if (withCode) {
            addField(R.string.pm_online_code, "").apply {
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                filters = arrayOf(InputFilter.AllCaps(), InputFilter.LengthFilter(5))
            }
        } else {
            null
        }
        private val relayInput = addField(R.string.pm_online_relay_server, prefs.getString(KEY_RELAY, null) ?: PMOnline.defaultRelay).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
        }

        val name get() = nameInput.text.toString().trim()
        val code get() = codeInput?.text?.toString()?.trim()?.uppercase().orEmpty()
        val relay get() = relayInput.text.toString().trim().ifEmpty { PMOnline.defaultRelay }

        fun save() {
            prefs.edit {
                putString(KEY_NAME, name)
                // Blank or default relay: keep following the community relay
                if (relay == PMOnline.defaultRelay) remove(KEY_RELAY) else putString(KEY_RELAY, relay)
            }
        }

        private fun addField(label: Int, value: String): EditText {
            root.addView(TextView(activity).apply { setText(label) })
            val input = EditText(activity).apply {
                setSingleLine()
                setText(value)
            }
            root.addView(input)
            return input
        }
    }

    private companion object {
        const val KEY_NAME = "name"
        const val KEY_RELAY = "relay"
    }
}
