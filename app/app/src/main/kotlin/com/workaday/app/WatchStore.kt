package com.workaday.app

import android.content.Context
import android.content.SharedPreferences
import com.workaday.core.ExchangeReport
import com.workaday.core.HealthSnapshot

/**
 * The two things that have to outlive the process: the health counters and the
 * address of the associated watch.
 *
 * **Device-protected storage, not the default.** The boot receiver is
 * `directBootAware` so that `LOCKED_BOOT_COMPLETED` reaches it at all — only
 * direct-boot-aware components get that broadcast — and the service it starts is
 * therefore running before the user has unlocked. Default `SharedPreferences`
 * live in credential-encrypted storage and *throw* until first unlock, so reading
 * health there would crash the service on exactly the path the whole
 * `LOCKED_BOOT_COMPLETED` requirement exists to protect. Neither value is
 * sensitive: a failure count and a MAC address the watch broadcasts in the clear.
 *
 * All the format lives in [HealthSnapshot.encode] / [HealthSnapshot.decode]
 * (Law 3 — the deciding happens in `core/`, the writing happens here). This class
 * owns one string and one string, and knows nothing about either.
 *
 * Every method here does disk I/O. Construct and use it off the main thread.
 */
class WatchStore private constructor(private val prefs: SharedPreferences) {

    /**
     * The address CompanionDeviceManager gave us, or null when no watch has been
     * associated yet.
     *
     * Written by the association flow and by the service's reconciliation
     * (`com.workaday.core.reconcileAssociation`), which also **clears** it when no
     * association backs it any more — a stored address with nothing behind it is a
     * phone that looks paired and can never connect. Until it is non-null there is
     * nothing to hand `connectGatt`, which is why `planStartup` refuses to start
     * the service.
     *
     * `commit()`, not `apply()`, for the same reason [writeHealth] uses it and one
     * more. The reason it shares: the process this value expects to die is its own,
     * and an OEM task killer can land between an `apply()` and its background
     * flush. The reason of its own: the very next thing the association flow does
     * is start the service, which reads this back — and "the write is durable
     * before the reader is told to look" is worth more than the microseconds.
     *
     * Both callers are already off the main thread.
     */
    var watchAddress: String?
        get() = prefs.getString(KEY_WATCH_ADDRESS, null)
        set(value) {
            prefs.edit().putString(KEY_WATCH_ADDRESS, value).commit()
        }

    /**
     * The last exchange, for the diagnostic screen — `Action.ReportExchange` made
     * durable.
     *
     * Read by an Activity that may be started long after the service that wrote
     * this was killed, which is exactly the situation the user opens it to look
     * into. [ExchangeReport.decode] is total and answers null for anything it does
     * not recognise, so a corrupt value costs one blank line rather than the one
     * window whose job is to explain what went wrong.
     */
    fun readLastExchange(): ExchangeReport? =
        ExchangeReport.decode(prefs.getString(KEY_LAST_EXCHANGE, null))

    /** Once per exchange, on the link's serial thread. See [writeHealth] on `commit`. */
    fun writeLastExchange(report: ExchangeReport) {
        prefs.edit().putString(KEY_LAST_EXCHANGE, report.encode()).commit()
    }

    /**
     * The counters, restored across process death.
     *
     * [HealthSnapshot.decode] is total — a missing, truncated, or foreign value
     * decodes to a zeroed snapshot rather than throwing — so a corrupt store
     * costs one extra retry instead of a service that will not start.
     */
    fun readHealth(): HealthSnapshot = HealthSnapshot.decode(prefs.getString(KEY_HEALTH, null))

    /**
     * `commit()`, not `apply()`, and deliberately.
     *
     * "Survives process death" is the requirement, and the process this one
     * expects to die is its own: an OEM task killer or a low-memory kill can land
     * between an `apply()` and its background flush. We are on the link's serial
     * thread here, never the main thread, so paying for the write is free — and
     * it happens once per exchange, roughly once an hour.
     */
    fun writeHealth(snapshot: HealthSnapshot) {
        prefs.edit().putString(KEY_HEALTH, snapshot.encode()).commit()
    }

    companion object {
        private const val NAME = "workaday-link"
        private const val KEY_HEALTH = "health"
        private const val KEY_WATCH_ADDRESS = "watch-address"
        private const val KEY_LAST_EXCHANGE = "last-exchange"

        /** Blocking. Call from a background thread. */
        fun open(context: Context): WatchStore {
            val storage = context.applicationContext.createDeviceProtectedStorageContext()
            return WatchStore(storage.getSharedPreferences(NAME, Context.MODE_PRIVATE))
        }
    }
}
