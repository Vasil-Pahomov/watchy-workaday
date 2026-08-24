package com.workaday.app

import android.util.Log

/**
 * One tag for the whole app, so `adb logcat -s Workaday` — the command
 * `CLAUDE.md` documents — shows everything and nothing else.
 *
 * There is deliberately no in-memory log buffer here. Law 2 requires bounded
 * memory in a process that lives for months, and the cheapest way to guarantee
 * that is to keep no buffer at all: logcat already has one, and it is the
 * platform's job to bound it. When APP-4's diagnostic screen needs recent events,
 * it gets them from `HealthSnapshot` and the machine's `lastStatus`, both of
 * which are fixed size by construction.
 */
internal const val LOG_TAG: String = "Workaday"

internal fun logInfo(message: String) {
    Log.i(LOG_TAG, message)
}
