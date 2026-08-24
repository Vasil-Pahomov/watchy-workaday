package com.workaday.app

import com.workaday.core.ExchangeOutcome
import com.workaday.core.OemBatteryAdvice
import com.workaday.core.SetupTask
import com.workaday.core.SetupVerdict
import com.workaday.core.SyncRecency
import com.workaday.core.protocol.SyncResult

/**
 * Enum to string resource, and nothing else.
 *
 * The same split as [ServiceNotification]: `core/` decides *which* case the app is
 * in, this file decides which words describe it. There is no `if` here that changes
 * what the app does, and adding a case to any of these enums breaks the matching
 * `when` at compile time rather than silently showing a blank line.
 *
 * The wording is aimed at whoever is wearing the watch, not at whoever wrote the
 * firmware: "the watch's clock did not get set" rather than "RtcWriteFailed".
 */
internal fun textFor(verdict: SetupVerdict): Int = when (verdict) {
    SetupVerdict.NotPaired -> R.string.verdict_not_paired
    SetupVerdict.Blocked -> R.string.verdict_blocked
    SetupVerdict.AtRisk -> R.string.verdict_at_risk
    SetupVerdict.Ready -> R.string.verdict_ready
}

internal fun textFor(task: SetupTask): Int = when (task) {
    SetupTask.AssociateWatch -> R.string.task_associate_watch
    SetupTask.GrantBluetoothPermission -> R.string.task_bluetooth_permission
    SetupTask.TurnBluetoothOn -> R.string.task_bluetooth_on
    SetupTask.ExemptFromBatteryOptimisation -> R.string.task_battery_exemption
    SetupTask.GrantNotificationPermission -> R.string.task_notifications
}

/**
 * The second half of `docs/background-execution.md` §2's product requirement —
 * "show whether it looks like it worked". [SyncRecency] describes elapsed time that
 * has already happened and these strings say no more than that.
 *
 * In particular none of them promises when the next attempt will be. The recovery
 * bound is one watchdog period and Doze stretches that by an amount nobody can
 * name, so a screen that said "retrying in twelve minutes" would be lying on any
 * phone that was asleep — which is all of them, most of the time.
 */
internal fun textFor(recency: SyncRecency): Int = when (recency) {
    SyncRecency.NeverSynced -> R.string.recency_never
    SyncRecency.Recent -> R.string.recency_recent
    SyncRecency.Overdue -> R.string.recency_overdue
    SyncRecency.Stalled -> R.string.recency_stalled
}

internal fun textFor(outcome: ExchangeOutcome): Int = when (outcome) {
    ExchangeOutcome.Succeeded -> R.string.outcome_succeeded
    ExchangeOutcome.WatchReportedFailure -> R.string.outcome_watch_refused
    ExchangeOutcome.MalformedStatus -> R.string.outcome_malformed
    ExchangeOutcome.OperationTimedOut -> R.string.outcome_timed_out
    ExchangeOutcome.DisconnectedMidExchange -> R.string.outcome_disconnected
    ExchangeOutcome.GattOperationFailed -> R.string.outcome_gatt_failed
    ExchangeOutcome.ConnectionAttemptFailed -> R.string.outcome_never_connected
    ExchangeOutcome.LocalClockUnusable -> R.string.outcome_local_clock
}

/**
 * PROTOCOL.md §3.2's `result` byte in wearer-facing terms.
 *
 * Non-null on purpose. There are two ways there is no [SyncResult] to show and they
 * are different things, so neither is folded in here: the watch never answered at
 * all (a timeout, a mid-exchange disconnect), or it answered with a code a newer
 * firmware knows and this build does not. The caller says which, and the second one
 * still shows the number.
 */
internal fun textFor(result: SyncResult): Int = when (result) {
    SyncResult.Ok -> R.string.result_ok
    SyncResult.BadLength -> R.string.result_bad_length
    SyncResult.BadVersion -> R.string.result_bad_version
    SyncResult.OutOfRange -> R.string.result_out_of_range
    SyncResult.RtcWriteFailed -> R.string.result_rtc_write_failed
    SyncResult.BadType -> R.string.result_bad_type
    SyncResult.Busy -> R.string.result_busy
}

/**
 * The per-vendor steps `docs/background-execution.md` §2 says the app must spell
 * out: "the app must tell the user what to do on their phone … this is a product
 * requirement, not an FAQ entry."
 *
 * Advice, not detection. No API reports whether an OEM battery manager is holding
 * the app down, so nothing in the app behaves differently on the strength of this;
 * the only evidence the user ever gets is [SyncRecency], measured from exchanges
 * that did or did not happen.
 */
internal fun textFor(advice: OemBatteryAdvice): Int = when (advice) {
    OemBatteryAdvice.Xiaomi -> R.string.oem_xiaomi
    OemBatteryAdvice.Huawei -> R.string.oem_huawei
    OemBatteryAdvice.Samsung -> R.string.oem_samsung
    OemBatteryAdvice.BbkGroup -> R.string.oem_bbk
    OemBatteryAdvice.Generic -> R.string.oem_generic
}
