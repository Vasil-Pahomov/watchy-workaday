package com.workaday.core

import kotlin.math.roundToInt

/**
 * What the always-on notification says while the app is at rest, decided.
 *
 * [ServiceNotice] answers *which situation the app is in*; five of its six values
 * are about the app itself and one word each is the whole of what there is to say.
 * [ServiceNotice.Waiting] is the odd one out: it is the state the app is in for
 * something like 99.9 % of its life, and "Ready for your watch" spends all of that
 * time saying nothing that could ever be false — which also means it says nothing
 * that could ever be useful. This is the two facts it says instead: what the watch
 * last told us its battery was, and when the watch was last actually synced.
 *
 * **Four cases and not one string with two blanks.** Either fact can be missing,
 * and they go missing independently: a watch that answered `BadVersion` reports a
 * battery percentage and never gets its clock set, and a phone that has caught a
 * window but whose watch has never taken a battery sample is the mirror image. A
 * single template with "unknown" substituted into it would put an awkward sentence
 * in front of the user in three cases out of four, and — more to the point — the
 * choice between the four is a decision, so it lives here rather than as three
 * `if`s inside a `Notification.Builder` chain where no test can reach them
 * (Law 3).
 *
 * **No elapsed time here, deliberately.** This carries the *instant* of the last
 * sync, not "four minutes ago". The notification is re-posted only when something
 * changes, and at rest nothing changes for an hour at a time (PROTOCOL.md §5.1),
 * so any pre-computed "four minutes ago" would still be on screen an hour later,
 * still saying four. `app/` hands the instant to the platform's own chronometer,
 * which counts on its own and cannot go stale. See `ServiceNotification`.
 */
sealed interface RestingSummary {

    /** 0–100 when the watch has ever reported one, null when it has not. */
    val batteryPercent: Int?

    /** A fresh install, or one whose records were lost. Nothing to report yet. */
    data object NothingYet : RestingSummary {
        override val batteryPercent: Int? get() = null
    }

    /**
     * Synced, but the watch has never sent a battery sample — PROTOCOL.md §3.2's
     * `0xFF`, which the decoder has already turned into a null by the time it gets
     * here.
     */
    data class SyncedOnly(val syncedAtUtcEpochSeconds: Long) : RestingSummary {
        override val batteryPercent: Int? get() = null
    }

    /**
     * The watch answered and told us its battery, but no exchange has ever
     * succeeded. Reachable, and worth its own wording: a §3.2 Status frame with a
     * non-zero `result` — `BadVersion` above all — carries a battery reading and is
     * still a failed exchange.
     */
    data class BatteryOnly(override val batteryPercent: Int) : RestingSummary

    /** Both facts. The ordinary case, and the one this change exists for. */
    data class BatteryAndSync(
        override val batteryPercent: Int,
        val syncedAtUtcEpochSeconds: Long,
    ) : RestingSummary

    /** The status-bar icon's fill, from [batteryPercent]. See [BatteryGauge]. */
    val gauge: BatteryGauge get() = batteryGaugeFor(batteryPercent)
}

/**
 * Assemble it from the two records the app persists — the same pair
 * [exchangeHistoryFor] reads, and through the same rule for which of them is
 * believed about the last success.
 *
 * The battery comes off [ExchangeReport] and only off it: the health counters do
 * not carry one, and a reading is a measurement rather than a running total. That
 * means it is the battery from the *last exchange*, successful or not — which is
 * exactly "the last measured battery" and is why a failed exchange that still got
 * an answer refreshes it.
 */
fun restingSummaryFor(health: HealthSnapshot, lastAttempt: ExchangeReport?): RestingSummary {
    val battery = lastAttempt?.batteryPercent
    val syncedAt = lastSuccessfulExchangeAt(health, lastAttempt)
    return when {
        battery != null && syncedAt != null -> RestingSummary.BatteryAndSync(battery, syncedAt)
        battery != null -> RestingSummary.BatteryOnly(battery)
        syncedAt != null -> RestingSummary.SyncedOnly(syncedAt)
        else -> RestingSummary.NothingYet
    }
}

/**
 * How full the watch drawn in the status bar is, in whole steps.
 *
 * The icon is a battery gauge for the *watch's* cell. A notification small icon is
 * rendered as an **alpha mask** and tinted by the system, so the reading has to be
 * carried by which pixels are opaque — there is no colour to spend and no room for
 * a number.
 *
 * **Why whole steps at all.** The gauge is a column of pixels a few millimetres
 * tall. Below some spacing two readings simply land on the same row, so a
 * percentage-exact fill would claim a precision the icon cannot draw. Quantising
 * says out loud what the icon is actually capable of.
 *
 * **Why ten, and where ten stops working.** The two places this icon appears are
 * rasterised at fixed sizes the framework declares: `status_bar_icon_size` is 22 dp
 * and `notification_header_icon_size` is 18 dp (android-35 `dimens.xml`). The gauge
 * column is 14/24 of that box — the case is sized to the real Watchy's proportions
 * and the gauge gets what is inside it — so a step is:
 *
 * | density | shade header (18 dp) | status bar (22 dp) |
 * |---|---|---|
 * | xhdpi (2×) | 2.1 px | 2.6 px |
 * | xxhdpi (3×) | 3.2 px | 3.9 px |
 * | xxxhdpi (4×) | 4.2 px | 5.1 px |
 *
 * One unit of that box is box/24 dp, **not** one dp — the whole 24-unit bitmap is
 * scaled into a box smaller than 24 dp. Re-deriving these cells on a "1 u = 1 dp"
 * reading inflates them by about 9 % in the status-bar column, which is the
 * direction that would make a finer step count look safe when it is not.
 *
 * The table is also a **framework-res-only reading**, and the status-bar column is
 * the optimistic end of it. `status_bar_icon_size` is the size of the *view*;
 * SystemUI has historically scaled the drawable inside it down again by
 * `status_bar_icon_drawing_size`, which is smaller and lives in SystemUI rather
 * than the platform tree, so it could not be checked here. If it applies, that
 * column shrinks by whatever that ratio is — of the order of 1.8 px rather than 2.6
 * at 2×. It does not change the conclusion below: still above one device pixel,
 * still not countable.
 *
 * `minSdk` is 31, and no device that satisfies it is below xhdpi — mdpi phones
 * stopped shipping around a decade before Android 12. So the real floor is the top
 * row, where every step is more than one device pixel and a change of ten points
 * visibly moves the fill. Ten steps hold there, but the margin is now thin: the
 * tightest corner in the whole design is the status bar at 2× *if* the drawing-size
 * caveat above applies, which is around 1.8 px. That is still above one device
 * pixel, so no two steps share a row — but it is the number to re-derive before
 * anything takes height away from the gauge again.
 *
 * Be honest about what that does **not** buy: at 18–22 dp nobody can *count* ten
 * steps, and the icon is not a segmented display where you read off "seven of ten".
 * It is a bar whose height is the charge, and ten steps is fine enough that it
 * reads as a level rather than as a handful of positions — which is the whole
 * reason to prefer ten over a coarser count.
 *
 * Ten is also **even**, and that matters: it makes half a battery exactly half a
 * case, which an odd count cannot do.
 */
sealed interface BatteryGauge {

    /**
     * No reading to draw: the watch has never reported one, or reported §3.2's
     * `0xFF` "no sample ever taken" — which the protocol decoder has already turned
     * into a null long before this.
     *
     * Kept apart from `Filled(0)` because an empty gauge is a *claim* — that the
     * watch is flat — and "we have not been told" is not that claim. `app/` draws
     * this as the case with a dash in it, which is the one mark that cannot be
     * mistaken for a fill: a fill is always anchored to the bottom edge.
     */
    data object NoReading : BatteryGauge

    /** [steps] of [STEPS] filled, from the bottom up. Always within `0..STEPS`. */
    data class Filled(val steps: Int) : BatteryGauge

    companion object {
        /** 10 % per step. See the note on [BatteryGauge] for why ten. */
        const val STEPS: Int = 10
    }
}

/**
 * The gauge for a reading: the nearest whole step, with one exception.
 *
 * **A watch with charge in it never draws empty.** 4 % is nearer to no steps than
 * to one, and an icon indistinguishable from a dead watch while the watch still has
 * hours left in it is the kind of wrong that gets a working watch put on charge —
 * or, worse, gets a *stopped app* mistaken for a flat battery, which is exactly the
 * confusion Law 2 spends the rest of its effort preventing. So the empty case is
 * reserved for a reading of zero and nothing else. At ten steps that exception
 * covers only 1–4 %, so it overstates by at most four points; it was three times
 * that when the gauge was coarser.
 *
 * Nearest, rather than rounding down throughout: rounding down would make the error
 * one-directional and up to a full step. The one place nearest costs something is
 * the top — 95 % and above draw full, overstating by at most five points — and the
 * direction that actually matters is handled by the exception above.
 *
 * @param batteryPercent 0–100, or null when there is no reading. Values outside the
 *   range are clamped rather than rejected: PROTOCOL.md §3.2 promises the sender
 *   normalises and both decoders hold it to that, so anything else here would be a
 *   bug in this app — and drawing a clamped gauge is a better response to one than
 *   throwing inside a notification build.
 */
fun batteryGaugeFor(batteryPercent: Int?): BatteryGauge {
    val percent = (batteryPercent ?: return BatteryGauge.NoReading).coerceIn(0, 100)
    if (percent == 0) return BatteryGauge.Filled(0)
    return BatteryGauge.Filled(
        (percent * BatteryGauge.STEPS / 100.0).roundToInt().coerceAtLeast(1),
    )
}
