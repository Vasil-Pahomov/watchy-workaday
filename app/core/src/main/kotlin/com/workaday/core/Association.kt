package com.workaday.core

/**
 * What the app should do about the CompanionDeviceManager records it can see.
 *
 * Every variant is either "here is the address to arm against" or "there is no
 * watch" — there is no variant that means "carry on with an address nothing backs".
 * That is the whole point of the file: `docs/background-execution.md` §1 says
 * associations survive reboot and are revoked only by uninstall, by
 * `disassociate()`, or by us, so a stored address with no surviving association is
 * a phone that **looks paired and can never connect**. Left alone it would arm a
 * `connectGatt` at a device the system will never let us reach, fail, back off,
 * and repeat for months while the notification says "Ready for your watch".
 */
sealed interface AssociationOutcome {

    /** The stored address is backed by a surviving association. Arm against it. */
    data class Confirmed(val address: String) : AssociationOutcome

    /**
     * Nothing usable was stored, but exactly one association survives. Write it
     * down and arm against it.
     *
     * Not a theoretical case: the association is created by the system before our
     * `onActivityResult` runs, so a process death in that window leaves the
     * association without the address. Adopting it is the difference between "open
     * the app and pair again" and "it just started working".
     */
    data class Adopted(val address: String) : AssociationOutcome

    /**
     * Something was stored and **no association backs it**. Clear it.
     *
     * Also covers a stored value that is not a Bluetooth address at all, which
     * `BluetoothAdapter.getRemoteDevice` would refuse for the life of the install.
     */
    data class Abandoned(val staleAddress: String) : AssociationOutcome

    /** Nothing stored and nothing to adopt. The user has not paired a watch. */
    data object Unpaired : AssociationOutcome

    /**
     * Nothing stored and more than one association survives, so there is no
     * non-arbitrary choice. Law 5 says one watch; guessing which one would be the
     * device registry Law 5 forbids, arrived at by accident.
     *
     * The user re-runs the association flow, which picks one and revokes the rest
     * ([supersededAssociations]).
     */
    data class Ambiguous(val addresses: List<String>) : AssociationOutcome
}

/**
 * `BluetoothAdapter.checkBluetoothAddress`, as a pure function, plus the
 * normalisation that stops the app storing an address it can never use.
 *
 * **This is not fussiness.** `BluetoothAdapter.getRemoteDevice(String)` accepts
 * `AA:BB:CC:DD:EE:FF` and throws `IllegalArgumentException` on
 * `aa:bb:cc:dd:ee:ff` — hex digits must be upper case. And the API-33 route out of
 * the association flow hands us `AssociationInfo.getDeviceMacAddress()`, an
 * `android.net.MacAddress`, whose `toString()` is **lower case**. So the obvious
 * implementation of "remember the watch we just paired" stores an address that
 * every later `connectGatt` refuses, the app backs off forever, and nothing in the
 * logs says why. The two halves of that trap live on opposite sides of the app,
 * which is exactly the kind of thing that belongs in one tested function.
 *
 * @return the address in the canonical upper-case form, or null if [raw] is not a
 *   Bluetooth address at all.
 */
fun normaliseBluetoothAddress(raw: String?): String? {
    val text = raw?.trim() ?: return null
    if (text.length != ADDRESS_LENGTH) return null
    for (i in 0 until ADDRESS_LENGTH) {
        val c = text[i]
        val ok = when (i % 3) {
            // Two hex digits, then a colon, five times over, then two more.
            2 -> c == ':'
            else -> c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F'
        }
        if (!ok) return null
    }
    // Locale-independent by construction: the accepted alphabet is ASCII hex, so
    // there is no Turkish-I to get wrong. Said out loud because uppercase() with a
    // default locale is a real bug in other alphabets.
    return buildString(ADDRESS_LENGTH) {
        for (c in text) append(if (c in 'a'..'f') c - LOWER_TO_UPPER else c)
    }
}

private const val ADDRESS_LENGTH = 17
private const val LOWER_TO_UPPER = 'a' - 'A'

/**
 * Reconcile what we wrote down with what the platform still remembers.
 *
 * Called on **every** service start — the boot path, the watchdog, and the
 * association flow — because that is the only moment the app looks at the world
 * at all. `docs/background-execution.md` §3: "Boot: receiver → start the service →
 * re-arm from the surviving CDM association. No re-pairing, no scan."
 *
 * @param storedAddress what [AssociationOutcome.Adopted] / the association flow
 *   last wrote. Not assumed to be normalised, or even to be an address.
 * @param associatedAddresses the addresses CompanionDeviceManager still has
 *   associations for, or **null meaning "we did not look"**.
 *
 *   That null is load-bearing and is the reason this parameter is not a plain
 *   list. CompanionDeviceManager's records are not documented as readable during
 *   direct boot, and the service starts during direct boot by design (Law 1's
 *   `LOCKED_BOOT_COMPLETED`). If "could not ask" collapsed into "the list is
 *   empty", every locked boot would silently un-pair the phone — turning the one
 *   path Law 1 exists to protect into the one path that breaks the app. So an
 *   absent answer confirms whatever is stored and clears nothing.
 */
fun reconcileAssociation(
    storedAddress: String?,
    associatedAddresses: List<String>?,
): AssociationOutcome {
    val stored = normaliseBluetoothAddress(storedAddress)
    if (storedAddress != null && stored == null) {
        // Not an address, so no association can ever back it and no connection can
        // ever use it. Safe to clear whether or not we can see the records.
        return AssociationOutcome.Abandoned(storedAddress)
    }

    val associated = associatedAddresses
        ?.mapNotNull(::normaliseBluetoothAddress)
        ?.distinct()

    if (associated == null) {
        return if (stored != null) AssociationOutcome.Confirmed(stored) else AssociationOutcome.Unpaired
    }

    if (stored != null) {
        return if (stored in associated) {
            AssociationOutcome.Confirmed(stored)
        } else {
            AssociationOutcome.Abandoned(stored)
        }
    }

    return when (associated.size) {
        0 -> AssociationOutcome.Unpaired
        1 -> AssociationOutcome.Adopted(associated.single())
        else -> AssociationOutcome.Ambiguous(associated)
    }
}

/**
 * The associations that are **not** the watch, once the user has picked one.
 *
 * Law 5, made real rather than merely asserted: this app talks to one peripheral,
 * so holding companion associations — and the background exemptions that come with
 * them — for devices it will never open a socket to is a registry of peripherals
 * by another name. It also keeps [reconcileAssociation] from ever having to answer
 * [AssociationOutcome.Ambiguous] in practice, since after a pairing there is
 * exactly one record left.
 *
 * Only called from the foreground association flow, immediately after the user has
 * chosen a device: revoking is destructive, and the moment when the user has just
 * said which watch is theirs is the only moment the answer is certain.
 *
 * Entries that are not addresses are left alone — we cannot act on a record we
 * cannot name, and a `disassociate` call built from garbage is worse than a stale
 * record.
 *
 * And if [chosen] itself is not an address, **nothing** is superseded. That branch
 * looks like paranoia and is not: the failure it refuses is revoking every real
 * association because the one we meant to keep was unreadable, which un-pairs the
 * phone in the same breath as pairing it and cannot be undone from here.
 */
fun supersededAssociations(chosen: String, all: List<String>): List<String> {
    val keep = normaliseBluetoothAddress(chosen) ?: return emptyList()
    return all
        .mapNotNull(::normaliseBluetoothAddress)
        .distinct()
        .filter { it != keep }
}
