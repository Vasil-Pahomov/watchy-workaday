package com.workaday.app

import com.workaday.core.BatteryGauge
import kotlin.math.max
import kotlin.math.min

/**
 * A rectangle in grid units — twenty-fourths of the icon.
 *
 * **Its own type, deliberately, rather than `android.graphics.RectF`.** The unit
 * tests here run against the stubbed `android.jar` with
 * `isReturnDefaultValues = true`, and under that setting `RectF`'s four-float
 * constructor is rewritten to a no-op: it does not throw, it simply leaves `left`,
 * `top`, `right` and `bottom` at zero. A geometry suite built on it would assert
 * against all-zero rectangles and either pass vacuously or fail for a reason that
 * has nothing to do with the icon. This is four floats and some division, touches
 * no framework type, and cannot be quietly emptied out.
 */
internal data class GridRect(
    val left: Float,
    val top: Float,
    val right: Float,
    val bottom: Float,
) {
    val width: Float get() = right - left
    val height: Float get() = bottom - top
    val centerX: Float get() = (left + right) / 2f
    val centerY: Float get() = (top + bottom) / 2f

    /** Shrunk by [by] on all four sides. */
    fun inset(by: Float): GridRect = GridRect(left + by, top + by, right - by, bottom - by)
}

/** The whole icon, one twenty-fourth at a time. */
internal const val GRID = 24f

/**
 * The vertical budget, and it is fixed: the glyph fills y 1..23 of the 24 u box,
 * leaving one unit of bleed top and bottom.
 *
 * So `22 = lug + case + lug` — a unit the lugs gain is a unit the case loses, and
 * the gauge loses it with the case. Every number below spends out of this.
 *
 * **The bleed stays unspent.** Growing the glyph to y 0.5..23.5 would buy the gauge
 * one more unit — 2.25 px per step instead of 2.10 at the 2x shade header, about
 * 7 % — and it is refused twice over: this icon would then stand 23 u tall beside
 * every other status-bar icon's 22, which reads as a bug rather than as a design;
 * and at a fixed lug height the extra unit lands on the *case*, taking it to
 * 14 x 18 = 0.778 and away from the device's proportions. Spending the bleed works
 * against the shape it would be spent for.
 */
private const val GLYPH_TOP = 1f
private const val GLYPH_BOTTOM = 23f

/**
 * The case, and the two numbers this shape is really made of.
 *
 * **Why 14 x 17 and not another size.** The ratio is the device's: 14 x 17 is 0.824
 * and the real Watchy v2 is 35.9 x 43.5 mm, which is 0.825. Fixing the case at the
 * proportions of the actual thing on the wrist is what makes it read as a watch
 * rather than as a box, and once the width is set the lug height is not a free
 * choice — it is whatever is left of the 22 u, which is 2.5 u each.
 *
 * **The scale was a real choice, and there was a rejected alternative.** 15 x 18
 * with 2.0 u lugs is 0.833 — also within a point of the device — and would have
 * left the gauge 15 u instead of 14, worth about 7 % more per step. It was turned
 * down as a half-measure: two thirds of the narrowing and 60 % of the lug height
 * that were actually asked for, against a brief of a silhouette that reads less
 * like a chunky box. Anyone wanting gauge height back should reach for that before
 * reaching for the bleed above — it is a one-line change, and it costs silhouette
 * rather than correctness.
 *
 * **The ratio agreement is on the centreline, not on what renders.** The stroke is
 * centred on the path, so the painted silhouette is 15.5 x 18.5 = 0.838 — about
 * 1.6 % off the device rather than 0.1 %. The change still moves toward the device
 * by roughly the claimed amount, since the painted shape was 0.854 before, but the
 * quoted precision belongs to the centreline.
 */
private val CASE = GridRect(left = 5f, top = 3.5f, right = 19f, bottom = 20.5f)

/**
 * The task's "roughly 3/4 of the width of the body's horizontal edge", and the one
 * proportion that has survived every revision of this shape unchanged.
 */
private const val LUG_WIDTH_FRACTION = 0.75f

/**
 * 1.5 u, centred on the path — so it overhangs the case by 0.75 u on every side,
 * and the painted glyph is 15.5 u wide rather than 14.
 *
 * That overhang is also why the lugs read as far taller than their nominal height
 * suggests: they clear the case's *painted* top edge by 1.75 u now against 0.75 u
 * before, which is +133 %, not the +67 % that 1.5 u to 2.5 u looks like on paper.
 * The second number is the one describing what anybody actually sees.
 */
internal const val STROKE = 1.5f

/** Enough to soften the corners without the case reading as a pill. */
internal const val CORNER_RADIUS = 1.5f

/** The "no reading" dash, as a fraction of the gauge's width. */
private const val DASH_WIDTH_FRACTION = 0.6f

/**
 * Everything the icon paints for one [BatteryGauge], in grid units.
 *
 * **Why this is a value and not eight expressions inside a `draw` method.** It used
 * to be the expressions, and the consequence was that six geometry numbers were
 * changed by hand — twice — with review as the only thing that ever checked them.
 * Inset the gauge by half the stroke instead of the whole stroke, anchor the fill
 * to the top instead of the bottom, or move the case by a unit, and 50 % stops
 * being the bottom half of the case, or the lugs stop being three quarters of its
 * width, or the dash drifts down until it can be mistaken for a fill — and every
 * test in the repository still passes. Pulled out here the arithmetic is pure, so
 * `WatchGlyphTest` holds each of those invariants directly.
 *
 * `app/build.gradle.kts` already makes the argument this file acts on: "Law 3 keeps
 * `core/` free of `android.*`; it does not say `app/` may go untested where a JVM
 * can reach it."
 *
 * [fill] and [dash] are mutually exclusive and either can be absent: a
 * [BatteryGauge.Filled] of zero steps paints no fill at all, and only
 * [BatteryGauge.NoReading] paints a dash.
 */
internal data class WatchGlyph(
    val case: GridRect,
    val topLug: GridRect,
    val bottomLug: GridRect,

    /**
     * The gauge column: the case inset by the **whole** stroke width rather than
     * half, so a clear gap is left between the case and the fill inside it. 14 u
     * tall and 11 u wide, and that 14 is the height [BatteryGauge.STEPS] is chosen
     * against — see the table there for what one step is in real pixels.
     */
    val inner: GridRect,

    /** Anchored to [inner]`.bottom` and growing upward, or null when empty. */
    val fill: GridRect?,

    /** Floating clear of [inner]`.bottom`, or null when there is a reading. */
    val dash: GridRect?,
) {
    /**
     * The outermost extent of anything painted, stroke overhang included — what has
     * to stay inside the 24 u box.
     */
    val paintedBounds: GridRect
        get() = GridRect(
            left = min(case.left - STROKE / 2f, min(topLug.left, bottomLug.left)),
            top = min(case.top - STROKE / 2f, topLug.top),
            right = max(case.right + STROKE / 2f, max(topLug.right, bottomLug.right)),
            bottom = max(case.bottom + STROKE / 2f, bottomLug.bottom),
        )
}

/** The glyph for a reading. Pure, total, and the only place the shape exists. */
internal fun watchGlyphFor(gauge: BatteryGauge): WatchGlyph {
    val lugInset = CASE.width * (1f - LUG_WIDTH_FRACTION) / 2f
    val inner = CASE.inset(STROKE)

    return WatchGlyph(
        case = CASE,
        topLug = GridRect(CASE.left + lugInset, GLYPH_TOP, CASE.right - lugInset, CASE.top),
        bottomLug = GridRect(CASE.left + lugInset, CASE.bottom, CASE.right - lugInset, GLYPH_BOTTOM),
        inner = inner,
        fill = (gauge as? BatteryGauge.Filled)
            ?.takeIf { it.steps > 0 }
            ?.let {
                val filled = inner.height * (it.steps.toFloat() / BatteryGauge.STEPS)
                GridRect(inner.left, inner.bottom - filled, inner.right, inner.bottom)
            },
        // A dash across the middle. The one mark that cannot be misread as a fill,
        // because a fill always touches the bottom edge and this never does — "we
        // have not been told" said in the only vocabulary an alpha mask has.
        dash = when (gauge) {
            BatteryGauge.NoReading -> {
                val half = inner.width * DASH_WIDTH_FRACTION / 2f
                GridRect(
                    left = inner.centerX - half,
                    top = inner.centerY - STROKE / 2f,
                    right = inner.centerX + half,
                    bottom = inner.centerY + STROKE / 2f,
                )
            }

            is BatteryGauge.Filled -> null
        },
    )
}
