package com.workaday.core

/**
 * The last exchange, written down where the diagnostic screen can find it.
 *
 * [Action.ReportExchange] is the machine saying "this is how it ended, and here is
 * what the watch said about it". Until now the Android layer only logged it, which
 * meant the one thing PROTOCOL.md §6.2 requires to be visible — the `result` code,
 * `BadVersion` above all — was visible only to whoever had `adb` plugged in.
 *
 * **Why a persisted record and not a field on a service or a ViewModel.** Law 1:
 * the Activity is a diagnostic window and nothing more, so it cannot be where this
 * lives; and the service that produced it may be dead by the time the user looks,
 * which is precisely the situation the user opens the screen to investigate. A
 * record that only exists while the thing being diagnosed is healthy is not a
 * diagnostic.
 *
 * **Why it is not folded into [HealthSnapshot].** That one is the input to Law 2's
 * escalation — it is read back at startup and drives the backoff. This one is read
 * by nothing but the screen. Keeping them apart means a format change here can
 * never cost the app its failure counters, and the watch's own battery reading
 * cannot end up as an argument to a retry decision.
 *
 * The whole storage contract is [encode] / [decode], as with [HealthSnapshot]: the
 * Android layer puts one string somewhere durable and hands it back.
 */
data class ExchangeReport(

    /** Wall clock when the exchange ended. Display only; nothing measures with it. */
    val atUtcEpochSeconds: Long,

    /** How it ended. */
    val outcome: ExchangeOutcome,

    /**
     * §3.2's `result` byte, or [HealthSnapshot.NO_RESULT_CODE] when the watch never
     * got as far as answering — a timeout, a disconnect mid-exchange, a malformed
     * frame. Kept raw so a code from a newer firmware can be shown as a number
     * rather than swallowed.
     */
    val statusResultCode: Int = HealthSnapshot.NO_RESULT_CODE,

    /** 0–100, or null for §3.2's `0xFF` "no sample ever taken" and for no answer. */
    val batteryPercent: Int? = null,

    /** §3.2's `fw_build`, or null when the watch did not answer. Diagnostic only. */
    val fwBuild: Int? = null,

    /** §3.2's `applied_utc_epoch_s` — what the watch committed; 0 if nothing was. */
    val appliedUtcEpochSeconds: Long = 0,
) {

    /** One line, version-tagged, no dependencies. */
    fun encode(): String = listOf(
        FORMAT_VERSION.toString(),
        atUtcEpochSeconds.toString(),
        outcome.name,
        statusResultCode.toString(),
        batteryPercent?.toString() ?: ABSENT,
        fwBuild?.toString() ?: ABSENT,
        appliedUtcEpochSeconds.toString(),
    ).joinToString(SEPARATOR)

    companion object {
        /** Bumped only if the field list changes. [decode] discards other versions. */
        const val FORMAT_VERSION: Int = 1

        private const val SEPARATOR = "|"
        private const val ABSENT = "-"
        private const val FIELD_COUNT = 7

        /**
         * Everything [Action.ReportExchange] carries, in the shape that survives a
         * process death.
         *
         * In `core/` rather than at the `when` branch that performs the action, so
         * that "what gets written down" is one tested mapping instead of six field
         * copies inside a `Service` (Law 3).
         */
        fun from(action: Action.ReportExchange): ExchangeReport {
            val status = action.status
            return ExchangeReport(
                atUtcEpochSeconds = action.atUtcEpochSeconds,
                outcome = action.outcome,
                statusResultCode = status?.resultCode ?: HealthSnapshot.NO_RESULT_CODE,
                batteryPercent = status?.batteryPercent,
                fwBuild = status?.fwBuild,
                appliedUtcEpochSeconds = status?.appliedUtcEpochSeconds ?: 0L,
            )
        }

        /**
         * The inverse of [encode], and total: **it never throws.**
         *
         * Null for nothing stored, a truncated write, a future format version, or
         * bytes from something else entirely. The screen then says "no exchange
         * recorded yet", which is honest, rather than the app failing to open the
         * one window whose job is to explain what went wrong.
         *
         * An unrecognised [ExchangeOutcome] name — a record written by a newer
         * build — is the one case that discards the whole record rather than a
         * field, because "how it ended" is the only thing on it that is not
         * optional.
         */
        fun decode(text: String?): ExchangeReport? {
            val parts = text?.split(SEPARATOR) ?: return null
            if (parts.size != FIELD_COUNT) return null
            if (parts[0].toIntOrNull() != FORMAT_VERSION) return null

            val at = parts[1].toLongOrNull() ?: return null
            val outcome = ExchangeOutcome.entries.firstOrNull { it.name == parts[2] } ?: return null
            val resultCode = parts[3].toIntOrNull() ?: return null
            val applied = parts[6].toLongOrNull() ?: return null

            return ExchangeReport(
                atUtcEpochSeconds = at.coerceAtLeast(0L),
                outcome = outcome,
                statusResultCode = resultCode,
                // A percentage outside 0..100 is dropped rather than shown:
                // PROTOCOL.md §3.2 promises the sender normalises, and the decoder
                // already held it to that promise once. This is the same promise
                // held across a restart.
                batteryPercent = parts[4].toIntOrNull()?.takeIf { it in 0..100 },
                fwBuild = parts[5].toIntOrNull(),
                appliedUtcEpochSeconds = applied.coerceAtLeast(0L),
            )
        }
    }
}
