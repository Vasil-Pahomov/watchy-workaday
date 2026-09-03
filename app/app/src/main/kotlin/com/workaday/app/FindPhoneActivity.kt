package com.workaday.app

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/**
 * The screen the phone shows while the watch is looking for it: what is
 * happening, and one big Stop button (PROTOCOL.md §4.1).
 *
 * Launched by the alarm notification's full-screen intent — over the lock screen,
 * with the display turned on — or by tapping the notification. It exists so that
 * someone who has just found their phone ringing on a table does not have to
 * unlock it and hunt for the notification to make it stop.
 *
 * **Law 1 shapes it exactly as it shapes [DiagnosticsActivity]:** it owns nothing
 * and decides nothing. The alarm is the service's; this screen sends one intent to
 * it and finishes. If the alarm ends first — the wearer pressed Back on the watch,
 * or the search timed out — the service says so with a package-scoped broadcast
 * and this screen finishes itself, so it can never sit there offering to stop
 * something that already has. And if it is resumed after the alarm has already
 * stopped, it finishes at once for the same reason.
 *
 * No UI framework, for the reasons the diagnostic screen gives.
 */
class FindPhoneActivity : Activity() {

    private val alarmEnded = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == FindPhoneAlarm.ACTION_FIND_ENDED) finish()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Both are also declared in the manifest; setting them here as well is
        // what the platform documents for an Activity started by a full-screen
        // intent, and it costs nothing.
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(buildLayout())
    }

    override fun onStart() {
        super.onStart()
        // The flags overload has existed since API 26 and RECEIVER_NOT_EXPORTED is
        // a compile-time int that inlines, so no API-33 guard — the same call and
        // the same reasoning as WatchLinkService.registerSystemEvents(). Lint's
        // InlinedApi warning here is the one it already raises there.
        registerReceiver(
            alarmEnded,
            IntentFilter(FindPhoneAlarm.ACTION_FIND_ENDED),
            Context.RECEIVER_NOT_EXPORTED,
        )
        // Resumed after the alarm already stopped — a stale notification tap, or
        // the watch ended the search while this was launching. Nothing to offer.
        if (!FindPhoneAlarm.isRinging) finish()
    }

    override fun onStop() {
        unregisterReceiver(alarmEnded)
        super.onStop()
    }

    private fun stopAlarm() {
        WatchLinkService.dismissFind(applicationContext)
        finish()
    }

    private fun buildLayout(): View {
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            val pad = dp(32)
            setPadding(pad, pad, pad, pad)
        }

        column.addView(
            TextView(this).apply {
                setText(R.string.find_title)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 26f)
                gravity = Gravity.CENTER
                setPadding(0, 0, 0, dp(16))
            },
            wrapContent(),
        )
        column.addView(
            TextView(this).apply {
                setText(R.string.find_text)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
                gravity = Gravity.CENTER
                setPadding(0, 0, 0, dp(32))
            },
            wrapContent(),
        )
        column.addView(
            Button(this).apply {
                setText(R.string.find_stop)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 24f)
                minimumHeight = dp(96)
                setOnClickListener { stopAlarm() }
            },
            wrapContent(),
        )
        column.addView(
            TextView(this).apply {
                setText(R.string.find_screen_hint)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                gravity = Gravity.CENTER
                setPadding(0, dp(32), 0, 0)
            },
            wrapContent(),
        )
        return column
    }

    private fun wrapContent() = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    )

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
