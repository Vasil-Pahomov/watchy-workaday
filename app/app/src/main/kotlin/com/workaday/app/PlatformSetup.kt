package com.workaday.app

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import com.workaday.core.SetupObservations

/**
 * Looking at the platform, and nothing else.
 *
 * Every function here answers one question with one framework call. Not a single
 * one of them decides anything: what the answers *mean* is
 * `com.workaday.core.assessSetup`, in `core/`, where a JVM test can drive it.
 *
 * **That split is a defence, not a style rule, and this is the file it defends
 * against.** `app/` compiles its unit tests against the stubbed `android.jar` with
 * `isReturnDefaultValues = true`, so every method below returns a zero to a JVM
 * test — and `PackageManager.PERMISSION_GRANTED` *is* zero. A test that reached
 * [granted] would be told the permission was held, whatever the code did, and would
 * pass while proving nothing. So the observations leave here as plain booleans and
 * the reasoning happens somewhere the stub cannot reach.
 *
 * All of it is cheap and none of it is cached: a permission can be revoked, and
 * Android auto-revokes the permissions of an app the user never opens — which is
 * this app's entire design premise.
 */
internal fun observeSetup(context: Context, watchAssociated: Boolean): SetupObservations =
    SetupObservations(
        watchAssociated = watchAssociated,
        bluetoothPermissionGranted = hasBluetoothConnectPermission(context),
        notificationPermissionGranted = hasNotificationPermission(context),
        bluetoothAdapterOn = isBluetoothOn(context),
        ignoringBatteryOptimisations = isIgnoringBatteryOptimisations(context),
    )

/** Without this there is no GATT at all — the whole app is inert. */
internal fun hasBluetoothConnectPermission(context: Context): Boolean =
    granted(context, Manifest.permission.BLUETOOTH_CONNECT)

/**
 * `POST_NOTIFICATIONS` is a runtime permission only from API 33; below that the
 * notification is simply shown, so there is nothing outstanding to ask for.
 */
internal fun hasNotificationPermission(context: Context): Boolean =
    Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
        granted(context, Manifest.permission.POST_NOTIFICATIONS)

internal fun isBluetoothOn(context: Context): Boolean =
    context.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true

/**
 * Whether the user has taken the app out of Doze's app-standby bucketing.
 *
 * Law 5 records why asking for this is legitimate here at all: the app is
 * sideloaded, not distributed through Play, whose policy restricts
 * `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` to a narrow set of categories. If that
 * ever changes, Law 5 changes first.
 */
internal fun isIgnoringBatteryOptimisations(context: Context): Boolean =
    context.getSystemService(PowerManager::class.java)
        ?.isIgnoringBatteryOptimizations(context.packageName) == true

/**
 * The system dialog that grants the exemption.
 *
 * Lint flags this as `BatteryLife` on the assumption the app is heading for Play.
 * It is not (Law 5), and `docs/background-execution.md` §1 lists the exemption
 * among the four things that let an app start a foreground service from the
 * background — one of exactly two this app has any claim to.
 */
internal fun batteryOptimisationRequestIntent(context: Context): Intent =
    Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
        .setData(Uri.fromParts("package", context.packageName, null))

/**
 * The fallback when the dialog above is not available — some builds route the
 * whole thing through a list instead. Also where the user goes to undo it.
 */
internal fun batteryOptimisationSettingsIntent(): Intent =
    Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)

/** Bluetooth is turned on by the user, in Settings. The app never turns a radio on. */
internal fun bluetoothSettingsIntent(): Intent = Intent(Settings.ACTION_BLUETOOTH_SETTINGS)

/**
 * This app's own entry in Settings, which is where every OEM buries its
 * background-execution switches (`docs/background-execution.md` §2). The wording
 * differs per vendor; the destination does not.
 */
internal fun appSettingsIntent(context: Context): Intent =
    Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
        .setData(Uri.fromParts("package", context.packageName, null))

private fun granted(context: Context, permission: String): Boolean =
    context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED
