package com.workaday.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.Icon
import com.workaday.core.BatteryGauge
import kotlin.math.roundToInt

/**
 * The status-bar icon: a Watchy, drawn, with its own battery as the fill.
 *
 * A rectangular case with a lug above and below it, each three quarters of the
 * case's width and centred on it — the silhouette of the thing on the user's
 * wrist, at the size Android gives a small icon. The case is the gauge: opaque
 * from the bottom up in the whole steps [BatteryGauge] decided, hollow above.
 *
 * **This file paints; it does not decide where.** Every coordinate comes from
 * [watchGlyphFor], which is pure and unit-tested — see [WatchGlyph] for why the
 * split exists and for the reasoning behind each number. All that happens here is
 * a multiplication by `u` and a handful of `drawRect` calls.
 *
 * **The case is drawn as tall as it can be**, because its inner height is the whole
 * budget [BatteryGauge.STEPS] is spent out of. The framework rasterises this at
 * 22 dp in the status bar and 18 dp in the notification header, not at the 24 dp
 * box it is authored in, so the gauge column is smaller than the icon and every
 * unit of it is worth having. Antialiasing is on and the system downscales with
 * filtering, so a step boundary that falls between device pixels renders as a
 * partly-covered row rather than vanishing — steps degrade into grey levels, never
 * into nothing.
 *
 * **A unit is not a dp.** The framework scales this whole 24 u bitmap into a box
 * *smaller* than 24 dp, so 1 u is box/24: about 0.92 dp in the status bar and
 * 0.75 dp in the shade, and the 22 u glyph measures roughly 20 dp and 16.5 dp
 * respectively, not 22. Do not size anything off a "1 u = 1 dp at status-bar size"
 * reading: a hairline drawn that way ships 9 % small, and re-deriving
 * [BatteryGauge]'s px-per-step table on it inflates every cell — which is exactly
 * the table the case for ten steps rests on.
 *
 * **Why this is drawn and not a drawable resource.** The fill is data, so a single
 * static drawable cannot express it, and the alternative — one pre-drawn vector per
 * step, plus one for "no reading" — puts the same geometry in twelve files with
 * nothing able to check that they still agree, and quietly hands the choice of step
 * size to however many files somebody is willing to hand-edit rather than to what
 * is legible at the size the framework actually draws this. That second objection
 * is not hypothetical: the step count has already been raised once, and it cost one
 * constant and one row of a table. Drawn, the shape exists once, the "three
 * quarters of the width" relationship is written down once, and the step count is
 * [BatteryGauge.STEPS] — the number `core/` decided and a JVM test pins.
 *
 * **Alpha, never colour.** A notification small icon is an alpha mask: the system
 * throws the colours away and tints whatever is opaque, white in the status bar and
 * the notification's accent in the shade. So everything here is drawn in opaque
 * white on transparent, and every distinction the icon makes — case, lugs, fill,
 * "no reading" — is a distinction between opaque and transparent. Nothing in this
 * file would survive being read as colour, and nothing in it needs to.
 *
 * There is no `if` here that changes what the app does: the glyph is a value
 * `core/` and [watchGlyphFor] between them have already settled.
 */
internal object WatchGaugeIcon {

    /**
     * A fresh bitmap per call, and deliberately no cache.
     *
     * It is about 20 kB of ARGB, it is built a handful of times an hour — the
     * notification is re-posted only when the notice or the summary changes — and
     * it is handed straight to the system, which keeps its own copy. A cache would
     * be a map that lives as long as the process (months, Law 2's "bounded
     * memory") to save an allocation that is not on any hot path.
     */
    fun of(context: Context, gauge: BatteryGauge): Icon {
        val size = (GRID * context.resources.displayMetrics.density).roundToInt().coerceAtLeast(GRID.toInt())
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        draw(Canvas(bitmap), size / GRID, watchGlyphFor(gauge))
        return Icon.createWithBitmap(bitmap)
    }

    /** @param u one twenty-fourth of the icon, in pixels. */
    private fun draw(canvas: Canvas, u: Float, glyph: WatchGlyph) {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }

        // Lugs first, solid: strap stubs are too small at this size to read as
        // outlines, and the case's stroke is drawn over where they meet it.
        paint.style = Paint.Style.FILL
        canvas.fill(glyph.topLug, u, paint)
        canvas.fill(glyph.bottomLug, u, paint)

        paint.style = Paint.Style.STROKE
        paint.strokeWidth = STROKE * u
        canvas.drawRoundRect(glyph.case.scaled(u), CORNER_RADIUS * u, CORNER_RADIUS * u, paint)

        paint.style = Paint.Style.FILL
        glyph.fill?.let { canvas.fill(it, u, paint) }
        glyph.dash?.let { canvas.fill(it, u, paint) }
    }

    private fun Canvas.fill(rect: GridRect, u: Float, paint: Paint) {
        drawRect(rect.left * u, rect.top * u, rect.right * u, rect.bottom * u, paint)
    }

    private fun GridRect.scaled(u: Float) = RectF(left * u, top * u, right * u, bottom * u)
}
