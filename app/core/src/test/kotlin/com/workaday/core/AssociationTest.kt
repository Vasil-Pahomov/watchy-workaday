package com.workaday.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * The two decisions behind "re-arm on boot from the surviving association, without
 * re-pairing".
 *
 * None of this is reachable on a JVM in its natural habitat — there is no
 * CompanionDeviceManager here, no reboot, and no user revoking an association in
 * Settings — which is exactly why it is a pure function of two observations rather
 * than a sequence of `if`s in a `Service`.
 *
 * What each test would have to break to fail is written above it, because the case
 * this file exists to catch is a phone that *looks* paired: an assertion that only
 * checked "we ended up with an address" would pass on the broken version too.
 */
class AssociationTest {

    private val watch = "AA:BB:CC:DD:EE:FF"
    private val other = "11:22:33:44:55:66"

    // ── normaliseBluetoothAddress ────────────────────────────────────────────

    @Test
    fun `a lower-case address is upper-cased, because getRemoteDevice refuses it otherwise`() {
        // Breaks if the normaliser is dropped or made a pass-through. This is the
        // exact value AssociationInfo.getDeviceMacAddress().toString() produces on
        // API 33+, and BluetoothAdapter.getRemoteDevice throws
        // IllegalArgumentException on it — so a pass-through here is an app that
        // pairs successfully and can then never connect.
        assertEquals("AA:BB:CC:DD:EE:FF", normaliseBluetoothAddress("aa:bb:cc:dd:ee:ff"))
    }

    @Test
    fun `an already canonical address is returned unchanged`() {
        assertEquals(watch, normaliseBluetoothAddress(watch))
    }

    @Test
    fun `mixed case and surrounding whitespace normalise`() {
        assertEquals("A1:B2:C3:D4:E5:F6", normaliseBluetoothAddress("  a1:B2:c3:D4:e5:F6  "))
    }

    @Test
    fun `digits are left alone`() {
        assertEquals("01:23:45:67:89:00", normaliseBluetoothAddress("01:23:45:67:89:00"))
    }

    @Test
    fun `anything that is not a Bluetooth address is rejected`() {
        // Each of these is accepted by a length-only or a case-only check, and each
        // of them makes BluetoothAdapter.getRemoteDevice throw.
        val rejected = listOf(
            null,
            "",
            "AA:BB:CC:DD:EE",             // too short
            "AA:BB:CC:DD:EE:FF:00",       // too long
            "AA:BB:CC:DD:EE:F",           // 16 chars
            "AA:BB:CC:DD:EE:FFF",         // 18 chars
            "AA-BB-CC-DD-EE-FF",          // wrong separator
            "AABBCCDDEEFF",               // no separators
            "GG:BB:CC:DD:EE:FF",          // G is not hex
            "AA:BB:CC:DD:EE:FG",          // one bad nibble, in the last field
            "AA:BB:CC:DD:EE:F\u0000", // right length, unprintable last nibble
            "the-watch",
        )
        for (raw in rejected) {
            assertNull(normaliseBluetoothAddress(raw), "accepted ${raw?.let { "\"$it\"" }}")
        }
    }

    @Test
    fun `a colon in the wrong place is rejected`() {
        assertNull(normaliseBluetoothAddress("AA:BB:CC:DD:EEF:F"))
        assertNull(normaliseBluetoothAddress(":AABB:CC:DD:EE:FF"))
    }

    // ── reconcileAssociation ─────────────────────────────────────────────────

    @Test
    fun `a stored address backed by an association is confirmed`() {
        assertEquals(
            AssociationOutcome.Confirmed(watch),
            reconcileAssociation(watch, listOf(watch)),
        )
    }

    @Test
    fun `case never decides whether the association still exists`() {
        // Breaks if either side of the comparison skips normalisation. The stored
        // value comes from our own SharedPreferences and the record list comes from
        // MacAddress.toString(), and on API 33+ those two are not the same case —
        // so a raw string comparison abandons a perfectly good association on every
        // single boot.
        assertEquals(
            AssociationOutcome.Confirmed(watch),
            reconcileAssociation("aa:bb:cc:dd:ee:ff", listOf("AA:BB:CC:DD:EE:FF")),
        )
        assertEquals(
            AssociationOutcome.Confirmed(watch),
            reconcileAssociation("AA:BB:CC:DD:EE:FF", listOf("aa:bb:cc:dd:ee:ff")),
        )
    }

    @Test
    fun `a stored address with no association left is abandoned, not kept`() {
        // THE case this file exists for. Breaks if the reconciliation is reduced to
        // "if something is stored, use it" — which is what the app did before this
        // change, and which leaves a phone that looks paired, arms an autoConnect
        // the system will never honour, and backs off forever.
        assertEquals(
            AssociationOutcome.Abandoned(watch),
            reconcileAssociation(watch, associatedAddresses = emptyList()),
        )
    }

    @Test
    fun `a stored address absent from a non-empty association list is abandoned`() {
        assertEquals(
            AssociationOutcome.Abandoned(watch),
            reconcileAssociation(watch, listOf(other)),
        )
    }

    @Test
    fun `an unreadable association list confirms what is stored and clears nothing`() {
        // Breaks if `null` (we did not look) is collapsed into `emptyList()` (there
        // is nothing there). The service starts during direct boot by design, and
        // CompanionDeviceManager's records are not documented as readable then — so
        // collapsing the two would un-pair the phone on every locked boot, turning
        // the one path Law 1 exists to protect into the one that breaks the app.
        assertEquals(
            AssociationOutcome.Confirmed(watch),
            reconcileAssociation(watch, associatedAddresses = null),
        )
    }

    @Test
    fun `an unreadable association list with nothing stored is simply unpaired`() {
        assertEquals(AssociationOutcome.Unpaired, reconcileAssociation(null, null))
    }

    @Test
    fun `nothing stored and one surviving association is adopted`() {
        // The process died between the system creating the association and our
        // onActivityResult writing the address down. Adopting is the difference
        // between "pair again" and "it already works".
        assertEquals(
            AssociationOutcome.Adopted(watch),
            reconcileAssociation(null, listOf("aa:bb:cc:dd:ee:ff")),
        )
    }

    @Test
    fun `nothing stored and nothing associated is unpaired`() {
        assertEquals(AssociationOutcome.Unpaired, reconcileAssociation(null, emptyList()))
    }

    @Test
    fun `nothing stored and two associations is ambiguous rather than a guess`() {
        assertEquals(
            AssociationOutcome.Ambiguous(listOf(watch, other)),
            reconcileAssociation(null, listOf(watch, other)),
        )
    }

    @Test
    fun `duplicate association records collapse before they can look ambiguous`() {
        // Breaks if distinct() is dropped: the same address reported twice is one
        // watch, and calling that ambiguous refuses to arm against a good
        // association.
        assertEquals(
            AssociationOutcome.Adopted(watch),
            reconcileAssociation(null, listOf(watch, "aa:bb:cc:dd:ee:ff")),
        )
    }

    @Test
    fun `association records that are not addresses are ignored`() {
        assertEquals(
            AssociationOutcome.Adopted(watch),
            reconcileAssociation(null, listOf("not-an-address", watch, "")),
        )
    }

    @Test
    fun `a stored value that is not an address is abandoned however the records look`() {
        // No association can ever back it and getRemoteDevice would refuse it for
        // the life of the install, so this one clears even when the records could
        // not be read at all.
        for (records in listOf(null, emptyList(), listOf(watch))) {
            assertEquals(
                AssociationOutcome.Abandoned("garbage"),
                reconcileAssociation("garbage", records),
                "records=$records",
            )
        }
    }

    @Test
    fun `an abandoned address is reported normalised so a log line names the real one`() {
        assertEquals(
            AssociationOutcome.Abandoned(watch),
            reconcileAssociation("aa:bb:cc:dd:ee:ff", emptyList()),
        )
    }

    @Test
    fun `over the whole input space, no outcome ever yields an unbacked address`() {
        // The property the individual cases are examples of, enumerated. An address
        // only ever comes back when the records back it or the records could not be
        // read; every other outcome leaves the app unpaired rather than armed at
        // something that cannot answer.
        val storedValues = listOf(null, watch, other, "aa:bb:cc:dd:ee:ff", "garbage", "")
        val recordSets = listOf(null, emptyList(), listOf(watch), listOf(watch, other), listOf("junk"))
        var cases = 0
        for (stored in storedValues) {
            for (records in recordSets) {
                val outcome = reconcileAssociation(stored, records)
                val label = "stored=$stored records=$records"
                val backing = records?.mapNotNull(::normaliseBluetoothAddress)?.distinct()
                when (outcome) {
                    is AssociationOutcome.Confirmed -> {
                        assertEquals(normaliseBluetoothAddress(stored), outcome.address, label)
                        if (backing != null) assertTrue(outcome.address in backing, label)
                    }

                    is AssociationOutcome.Adopted -> {
                        assertNull(normaliseBluetoothAddress(stored), label)
                        assertEquals(listOf(outcome.address), backing, label)
                    }

                    is AssociationOutcome.Abandoned ->
                        assertTrue(stored != null, label)

                    AssociationOutcome.Unpaired ->
                        assertNull(normaliseBluetoothAddress(stored), label)

                    is AssociationOutcome.Ambiguous -> {
                        assertNull(normaliseBluetoothAddress(stored), label)
                        assertTrue(outcome.addresses.size > 1, label)
                    }
                }
                cases++
            }
        }
        assertEquals(storedValues.size * recordSets.size, cases)
    }

    // ── supersededAssociations ───────────────────────────────────────────────

    @Test
    fun `the chosen watch is never superseded, whatever case it arrives in`() {
        // Breaks if the filter compares raw strings. Revoking the association the
        // user just made is the worst outcome in this file: the app would pair,
        // un-pair itself in the same breath, and report no watch.
        assertEquals(emptyList<String>(), supersededAssociations(watch, listOf("aa:bb:cc:dd:ee:ff")))
    }

    @Test
    fun `every other association is superseded`() {
        assertEquals(listOf(other), supersededAssociations(watch, listOf(watch, other)))
    }

    @Test
    fun `nothing to revoke when the chosen watch is the only association`() {
        assertEquals(emptyList<String>(), supersededAssociations(watch, listOf(watch)))
    }

    @Test
    fun `records that are not addresses are left alone rather than revoked blindly`() {
        assertEquals(listOf(other), supersededAssociations(watch, listOf(watch, other, "junk")))
    }

    @Test
    fun `duplicates are revoked once`() {
        assertEquals(
            listOf(other),
            supersededAssociations(watch, listOf(other, "11:22:33:44:55:66")),
        )
    }

    @Test
    fun `an unnameable choice supersedes nothing at all`() {
        // Breaks if the null-chosen guard is dropped: the filter would then match
        // nothing, every real association would be revoked, and the phone would be
        // un-paired with no way back from here.
        assertEquals(emptyList<String>(), supersededAssociations("garbage", listOf(watch, other)))
    }
}
