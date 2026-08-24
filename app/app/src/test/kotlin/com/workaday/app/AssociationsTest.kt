package com.workaday.app

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * The wiring around the association decisions — tested where it actually lives.
 *
 * `AssociationTest` in `core/` covers the decisions: which outcome, which address,
 * which records are superseded. It cannot cover any of the four properties below,
 * because each of them is a *consequence* rather than a conclusion, and all four
 * survived a mutation run with every one of the 196 `core/` tests green — including
 * the ones whose names sound exactly like cover. That is the same gap
 * `WatchLinkDeliveryTest` exists to close for `WatchLink`, and it is closed the
 * same way: the collaborators are constructor or parameter seams, and the
 * production wiring is a factory.
 *
 * The four:
 *
 * - the direct-boot gate must mean *we never ask*, not *we ignore the answer*;
 * - [AssociationOutcome.Abandoned][com.workaday.core.AssociationOutcome.Abandoned]
 *   must clear the stored address, or `Association.kt` is a no-op;
 * - [AssociationOutcome.Adopted][com.workaday.core.AssociationOutcome.Adopted]
 *   must persist, or the recovery lasts exactly one process;
 * - pairing must revoke the *superseded* records, not the observed ones.
 *
 * **None of this touches a stubbed framework value**, which matters in this module:
 * `isReturnDefaultValues = true` makes every `android.*` method return zero, and
 * `PackageManager.PERMISSION_GRANTED` is zero. Nothing below asks the framework
 * anything. The only Android call reached at all is `android.util.Log`, from the
 * logging inside [reArmedWatchAddress], and no assertion depends on it.
 */
class AssociationsTest {

    private val watch = "AA:BB:CC:DD:EE:FF"
    private val other = "11:22:33:44:55:66"

    // ── The direct-boot gate ─────────────────────────────────────────────────

    @Test
    fun `before the first unlock the platform is never even asked`() {
        // The whole property, and deliberately the *only* thing asserted about it.
        // The gate exists because whether CompanionDeviceManager's records are
        // readable during direct boot is not documented anywhere — so "we never
        // ask" is a claim this machine can settle, and "the answer would have been
        // wrong" is not.
        var asked = 0
        val observed = Associations.observed(userUnlocked = false) {
            asked++
            listOf(watch)
        }
        assertEquals(0, asked, "the records were read before the first unlock")
        assertNull(observed, "a locked boot must report 'we do not know', never a list")
    }

    @Test
    fun `after the first unlock the reader's answer is forwarded verbatim`() {
        var asked = 0
        val observed = Associations.observed(userUnlocked = true) {
            asked++
            listOf(watch, other)
        }
        assertEquals(1, asked, "the records must be read exactly once")
        assertEquals(listOf(watch, other), observed)
    }

    @Test
    fun `an absent answer after unlock stays absent, and an empty one stays empty`() {
        // These two are the difference between "we could not look" and "there is
        // nothing there", and reconcileAssociation treats them oppositely: the
        // first confirms the stored address, the second clears it. Collapsing
        // either into the other is a silent un-pairing in one direction and a
        // phone that looks paired forever in the other.
        assertNull(Associations.observed(userUnlocked = true) { null })
        assertEquals(emptyList(), Associations.observed(userUnlocked = true) { emptyList() })
    }

    // ── Performing the reconciliation ────────────────────────────────────────

    /** Records every write so a test can assert that there was not one. */
    private class Writes {
        val values = mutableListOf<String?>()
        val write: (String?) -> Unit = { values += it }
    }

    @Test
    fun `a confirmed association arms the stored address and writes nothing`() {
        val writes = Writes()
        val address = reArmedWatchAddress(watch, listOf(watch), writes.write)
        assertEquals(watch, address)
        assertEquals(emptyList(), writes.values, "a confirmed address must not be rewritten on every start")
    }

    @Test
    fun `an abandoned association clears the stored address`() {
        // Delete `writeAddress(null)` and this is the only test in the repo that
        // notices — the whole of Association.kt becomes a no-op, the phone keeps
        // looking paired, and it arms at an address no association backs.
        val writes = Writes()
        val address = reArmedWatchAddress(watch, associatedAddresses = emptyList(), writeAddress = writes.write)
        assertNull(address, "an abandoned watch must not be armed against")
        assertEquals(listOf<String?>(null), writes.values, "the stale address was left in the store")
    }

    @Test
    fun `an adopted association is written down, not merely returned`() {
        // Returning without persisting arms this process and no other: BootReceiver
        // and WatchdogWorker both gate on the stored address, so planStartup would
        // answer NoWatchPaired and the service would never come back after a
        // reboot. The test that sounds like cover — `nothing stored and one
        // surviving association is adopted` — cannot see any of that.
        val writes = Writes()
        val address = reArmedWatchAddress(storedAddress = null, associatedAddresses = listOf(watch), writeAddress = writes.write)
        assertEquals(watch, address)
        assertEquals(listOf<String?>(watch), writes.values)
    }

    @Test
    fun `what is adopted is the normalised address, not the platform's lower case`() {
        // MacAddress.toString() is lower case and getRemoteDevice throws on it, so
        // persisting the raw record would store an address every later connectGatt
        // refuses — a phone that pairs and then fails forever, with nothing in the
        // log to say why.
        val writes = Writes()
        val address = reArmedWatchAddress(null, listOf("aa:bb:cc:dd:ee:ff"), writes.write)
        assertEquals(watch, address)
        assertEquals(listOf<String?>(watch), writes.values)
    }

    @Test
    fun `a blind read confirms what is stored and writes nothing`() {
        val writes = Writes()
        val address = reArmedWatchAddress(watch, associatedAddresses = null, writeAddress = writes.write)
        assertEquals(watch, address)
        assertEquals(emptyList(), writes.values, "an unread record set must never cause a write")
    }

    @Test
    fun `an unpaired app arms nothing and writes nothing`() {
        val writes = Writes()
        assertNull(reArmedWatchAddress(null, emptyList(), writes.write))
        assertNull(reArmedWatchAddress(null, null, writes.write))
        assertEquals(emptyList(), writes.values)
    }

    @Test
    fun `ambiguous records arm nothing and write nothing`() {
        // Picking one would be the device registry Law 5 forbids, and writing one
        // down would make the guess permanent.
        val writes = Writes()
        assertNull(reArmedWatchAddress(null, listOf(watch, other), writes.write))
        assertEquals(emptyList(), writes.values)
    }

    @Test
    fun `a garbage stored value is cleared even when the records could not be read`() {
        val writes = Writes()
        assertNull(reArmedWatchAddress("not-an-address", null, writes.write))
        assertEquals(listOf<String?>(null), writes.values)
    }

    // ── Completing a pairing ─────────────────────────────────────────────────

    /** The three effects, in the order they were performed. */
    private class Pairing {
        val steps = mutableListOf<String>()
        var revoked: List<String>? = null

        fun run(address: String, associated: List<String>?) = completePairing(
            address = address,
            associatedAddresses = associated,
            writeAddress = { steps += "write $it" },
            revoke = { revoked = it; steps += "revoke $it" },
            startService = { steps += "start" },
        )
    }

    @Test
    fun `the address is written before the service is told to look`() {
        // Reverse the two and the service comes up, finds no address, logs "no
        // associated watch" and stops itself — so the pairing the user just
        // completed does not take until the watchdog's next period, which Doze can
        // stretch well past it.
        val pairing = Pairing()
        pairing.run(watch, listOf(watch))
        assertEquals("write $watch", pairing.steps.first())
        assertEquals("start", pairing.steps.last())
    }

    @Test
    fun `only the associations that are not this watch are revoked`() {
        // Hand `revoke` the observed list instead of supersededAssociations' answer
        // and pairing revokes the watch it has just paired. `core/`'s `the chosen
        // watch is never superseded` cannot see which list the call site passes.
        val pairing = Pairing()
        pairing.run(watch, listOf(watch, other))
        assertEquals(listOf(other), pairing.revoked)
    }

    @Test
    fun `the watch just paired is never revoked, whatever case the records use`() {
        val pairing = Pairing()
        pairing.run(watch, listOf("aa:bb:cc:dd:ee:ff", other))
        assertEquals(listOf(other), pairing.revoked)
        assertTrue(pairing.revoked!!.none { it.equals(watch, ignoreCase = true) })
    }

    @Test
    fun `records that could not be read revoke nothing`() {
        // "We do not know" must not become "give all of them up".
        val pairing = Pairing()
        pairing.run(watch, associated = null)
        assertEquals(emptyList(), pairing.revoked)
        assertEquals(listOf("write $watch", "revoke []", "start"), pairing.steps)
    }

    @Test
    fun `a first pairing on a clean phone writes, revokes nothing, and starts`() {
        val pairing = Pairing()
        pairing.run(watch, listOf(watch))
        assertEquals(listOf("write $watch", "revoke []", "start"), pairing.steps)
    }
}
