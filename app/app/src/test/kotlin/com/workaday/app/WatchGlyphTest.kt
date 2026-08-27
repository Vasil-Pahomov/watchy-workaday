package com.workaday.app

import com.workaday.core.BatteryGauge
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * The icon's geometry, held to the shape the specification actually states.
 *
 * This file exists because of what the geometry did *not* have. `RestingSummaryTest`
 * covers the arithmetic — which step a percentage lands on — and its assertions are
 * written in percentages and `STEPS`, so they cannot see a rectangle. Meanwhile six
 * geometry numbers were revised by hand across two rounds, and every one of the
 * properties below could have been broken by any of those edits with the whole
 * suite still green: inset the gauge by half the stroke instead of the whole
 * stroke, anchor the fill to the top, move the case a unit, and 50 % quietly stops
 * being the bottom half of the case.
 *
 * That is the same defect shape `ExchangeHistoryTest` was written for — a property
 * that lives *between* values, with nowhere to be asserted — and the same repair:
 * make the thing a value ([WatchGlyph]) so a test can look at it.
 *
 * Everything is in grid units, twenty-fourths of the icon, exactly as the comments
 * in `WatchGlyph.kt` are.
 */
class WatchGlyphTest {

    /** Grid units are small numbers with exact halves; this is float noise only. */
    private val tolerance = 0.0001f

    private val full = watchGlyphFor(BatteryGauge.Filled(BatteryGauge.STEPS))
    private val empty = watchGlyphFor(BatteryGauge.Filled(0))
    private val noReading = watchGlyphFor(BatteryGauge.NoReading)

    // ── The silhouette ───────────────────────────────────────────────────────

    @Test
    fun `each lug is three quarters of the case's width`() {
        // The one proportion that has survived every revision of this shape, and
        // the one the task stated outright. It is derived from the case's width, so
        // narrowing the case must narrow the lugs with it — this is what says so.
        for ((name, lug) in listOf("top" to full.topLug, "bottom" to full.bottomLug)) {
            assertEquals(0.75f * full.case.width, lug.width, tolerance, name)
        }
    }

    @Test
    fun `the lugs are centred on the case`() {
        // Off-centre lugs would read as a watch worn crooked, and at 22 dp it would
        // look like a rendering bug rather than a design.
        for ((name, lug) in listOf("top" to full.topLug, "bottom" to full.bottomLug)) {
            assertEquals(full.case.centerX, lug.centerX, tolerance, name)
        }
    }

    @Test
    fun `lug plus case plus lug is the whole 22 unit budget`() {
        // The constraint every other number spends against: the glyph fills y 1..23
        // of the 24 u box, so a unit the lugs gain is a unit the case — and the
        // gauge inside it — loses. If this ever fails, something grew without
        // anything else having been asked to pay for it.
        assertEquals(1f, full.topLug.top, tolerance, "glyph starts at y 1")
        assertEquals(23f, full.bottomLug.bottom, tolerance, "glyph ends at y 23")
        assertEquals(
            22f,
            full.topLug.height + full.case.height + full.bottomLug.height,
            tolerance,
        )
        // The lugs meet the case rather than floating off it or overlapping it.
        assertEquals(full.case.top, full.topLug.bottom, tolerance)
        assertEquals(full.case.bottom, full.bottomLug.top, tolerance)
    }

    @Test
    fun `nothing paints outside the 24 unit box`() {
        // Counting the stroke overhang: it is centred on the path, so the case
        // bleeds half a stroke past its own rectangle on all four sides. Clipping
        // would not throw — it would silently shave the outline in the status bar.
        for ((name, glyph) in cases()) {
            val bounds = glyph.paintedBounds
            assertTrue(bounds.left >= 0f, "$name left ${bounds.left}")
            assertTrue(bounds.top >= 0f, "$name top ${bounds.top}")
            assertTrue(bounds.right <= GRID, "$name right ${bounds.right}")
            assertTrue(bounds.bottom <= GRID, "$name bottom ${bounds.bottom}")
        }
        // And the overhang is really being counted, so this test cannot pass by
        // having quietly forgotten it.
        assertEquals(full.case.left - STROKE / 2f, full.paintedBounds.left, tolerance)
        assertEquals(full.case.right + STROKE / 2f, full.paintedBounds.right, tolerance)
    }

    // ── The gauge ────────────────────────────────────────────────────────────

    @Test
    fun `half the steps is exactly the bottom half of the case`() {
        // The geometric half of `RestingSummaryTest`'s "half is exactly half", which
        // can only see that 50 % maps to STEPS / 2. This is the half that says
        // STEPS / 2 of the gauge is the bottom half of the *case*: three centres
        // that must coincide, and do not if the gauge is inset asymmetrically or
        // the fill is anchored to the wrong edge.
        val half = watchGlyphFor(BatteryGauge.Filled(BatteryGauge.STEPS / 2))
        val fill = assertNotNull(half.fill)

        assertEquals(half.inner.centerY, fill.top, tolerance, "fill top vs gauge centre")
        assertEquals(half.case.centerY, fill.top, tolerance, "fill top vs case centre")
        assertEquals(fill.height, half.inner.height / 2f, tolerance, "fill is half the gauge")
    }

    @Test
    fun `the fill is anchored to the bottom and grows upward`() {
        // "Bottom-up" is the whole readability of the gauge: an empty case means
        // flat and a full one means charged. Anchored to the top it would still
        // animate with the battery and mean the opposite.
        var previousTop = Float.MAX_VALUE
        for (steps in 1..BatteryGauge.STEPS) {
            val fill = assertNotNull(watchGlyphFor(BatteryGauge.Filled(steps)).fill, "$steps")
            assertEquals(full.inner.bottom, fill.bottom, tolerance, "$steps steps: bottom moved")
            assertEquals(full.inner.left, fill.left, tolerance, "$steps steps: left moved")
            assertEquals(full.inner.right, fill.right, tolerance, "$steps steps: right moved")
            assertTrue(fill.top < previousTop, "$steps steps: fill did not grow upward")
            previousTop = fill.top
        }
    }

    @Test
    fun `a full gauge is the whole inner area and an empty one paints nothing`() {
        assertEquals(full.inner, assertNotNull(full.fill), "full fill should be the gauge itself")
        // Not a zero-height rectangle: nothing at all. A zero-height fill would
        // paint nothing today and become a hairline the moment anyone rounded it.
        assertNull(empty.fill)
        assertNull(empty.dash, "an empty gauge is a reading, not a missing one")
    }

    @Test
    fun `the gauge is inset by the whole stroke, leaving a gap inside the case`() {
        // Inset by half the stroke, the fill would touch the inside of the outline
        // and the two would read as one blob at status-bar size. This is also the
        // number BatteryGauge.STEPS is sized against: 14 u tall.
        assertEquals(full.case.left + STROKE, full.inner.left, tolerance)
        assertEquals(full.case.top + STROKE, full.inner.top, tolerance)
        assertEquals(full.case.right - STROKE, full.inner.right, tolerance)
        assertEquals(full.case.bottom - STROKE, full.inner.bottom, tolerance)
        assertEquals(14f, full.inner.height, tolerance, "the height the step count is chosen against")
    }

    // ── "No reading" ─────────────────────────────────────────────────────────

    @Test
    fun `the no-reading dash floats clear of the bottom edge`() {
        // The single property that keeps "we have not been told" from being read as
        // "the watch is flat": every fill touches inner.bottom, and this must not.
        val dash = assertNotNull(noReading.dash)
        assertTrue(
            dash.bottom < noReading.inner.bottom,
            "dash bottom ${dash.bottom} must be strictly above gauge bottom ${noReading.inner.bottom}",
        )
        // Strictly above is the requirement; comfortably above is what it should be.
        // A dash within a step of the bottom would be a sliver-fill to the eye.
        val stepHeight = noReading.inner.height / BatteryGauge.STEPS
        assertTrue(
            noReading.inner.bottom - dash.bottom > stepHeight,
            "dash sits within one step of the bottom and would read as a low fill",
        )
        assertEquals(noReading.inner.centerY, dash.centerY, tolerance, "dash is centred")
        assertNull(noReading.fill, "a dash and a fill are mutually exclusive")
    }

    @Test
    fun `no reading is the only case that draws a dash`() {
        for (steps in 0..BatteryGauge.STEPS) {
            assertNull(watchGlyphFor(BatteryGauge.Filled(steps)).dash, "$steps steps")
        }
        assertNotNull(noReading.dash)
    }

    @Test
    fun `the case, the lugs and the gauge do not move with the reading`() {
        // Only the fill and the dash depend on the gauge. If a later edit made the
        // case shrink for low readings, say, the icon would jitter in the status bar
        // once an hour and nothing else here would notice.
        for ((name, glyph) in cases()) {
            assertEquals(full.case, glyph.case, name)
            assertEquals(full.topLug, glyph.topLug, name)
            assertEquals(full.bottomLug, glyph.bottomLug, name)
            assertEquals(full.inner, glyph.inner, name)
        }
    }

    private fun cases(): List<Pair<String, WatchGlyph>> =
        (0..BatteryGauge.STEPS).map { "$it steps" to watchGlyphFor(BatteryGauge.Filled(it)) } +
            ("no reading" to noReading)
}
