package wc3.recomp.overlaycanvas

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BlurMaskFilter
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.CornerPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PorterDuff
import android.graphics.PorterDuffColorFilter
import android.graphics.PorterDuffXfermode
import android.graphics.Rect
import android.graphics.Xfermode
import java.io.IOException
import kotlin.math.ceil

/**
 * Builds the layer bitmaps used by the C overlay compositor.
 *
 * The C side passes geometry, scale, effective corner radius, label text and
 * the standard overlay icon id.  Icons are read from the same
 * `inputcontrols/icons/<id>.png` asset set as the Winlator control editor.
 * This helper rasterises the control layers once with Android Canvas; the C
 * hot path then blits the cached atlas for every frame.  No Java code runs
 * during rendering, visual-state changes or touch handling.
 */
object OverlayCanvasCache {
    private const val TYPE_BUTTON = 0
    private const val TYPE_DPAD = 1
    private const val SHAPE_RECT = 0
    private const val SHAPE_CIRCLE = 1

    private const val SLOT_FILL = 0
    private const val SLOT_GLOW = 1
    private const val SLOT_STROKE_TEXT = 2
    private const val SLOT_OUTER = 3
    private const val SLOT_DPAD_ACTIVE = 4
    private const val SLOT_DPAD_INACTIVE = 5

    private val XFER_CLEAR: Xfermode =
        PorterDuffXfermode(PorterDuff.Mode.CLEAR)
    private val XFER_DST_OUT: Xfermode =
        PorterDuffXfermode(PorterDuff.Mode.DST_OUT)

    private class Layer(
        val element: Int,
        val slot: Int,
        val width: Int,
        val height: Int,
        val dstX: Int,
        val dstY: Int,
        val bitmap: Bitmap
    ) {
        var srcX: Int = 0
        var srcY: Int = 0
    }

    @JvmStatic
    fun buildAtlas(
        context: Context,
        width: Int,
        height: Int,
        snappingSize: Int,
        strokeWidthMultiplier: Float,
        elementCount: Int,
        meta: IntArray?,
        params: FloatArray?,
        labels: Array<String?>?,
        icons: IntArray?,
        layerOut: IntArray
    ): Bitmap {
        if (meta == null || params == null || elementCount <= 0) {
            return Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888)
        }
        if (layerOut.isNotEmpty()) layerOut[0] = 0

        val strokeWidth = snappingSize * strokeWidthMultiplier
        val strokePad = maxOf(1, ceil(strokeWidth * 0.5f).toInt() + 1)
        val layers = ArrayList<Layer>()
        val iconCache = HashMap<Int, Bitmap?>()

        for (i in 0 until elementCount) {
            val base = i * 6
            val type = meta[base + 0]
            val shape = meta[base + 1]
            val box = Rect(
                meta[base + 2], meta[base + 3],
                meta[base + 4], meta[base + 5]
            )
            if (box.width() <= 0 || box.height() <= 0) continue
            val scale = params[i * 2]
            val cornerRadius = params[i * 2 + 1]
            val text = labels?.getOrNull(i).orEmpty()
            val iconId = icons?.getOrNull(i) ?: 0
            val icon = if (iconId > 0) {
                if (iconCache.containsKey(iconId)) {
                    iconCache[iconId]
                } else {
                    loadIcon(context, iconId).also {
                        iconCache[iconId] = it
                    }
                }
            } else {
                null
            }

            if (type == TYPE_DPAD) {
                addDPadLayers(
                    layers, i, box, snappingSize, scale, cornerRadius,
                    strokeWidth, strokePad
                )
            } else if (type == TYPE_BUTTON) {
                addButtonLayers(
                    layers, i, box, snappingSize, scale, shape,
                    cornerRadius, strokeWidth, strokePad, text, icon
                )
            }
        }

        var atlasWidth = 16
        for (layer in layers) atlasWidth = maxOf(atlasWidth, layer.width)
        var x = 0
        var y = 0
        var rowHeight = 0
        for (layer in layers) {
            if (x > 0 && x + layer.width > atlasWidth) {
                x = 0
                y += rowHeight
                rowHeight = 0
            }
            layer.srcX = x
            layer.srcY = y
            x += layer.width
            if (layer.height > rowHeight) rowHeight = layer.height
        }
        val atlasHeight = maxOf(16, y + rowHeight)

        val atlas = Bitmap.createBitmap(
            atlasWidth, atlasHeight, Bitmap.Config.ARGB_8888
        )
        val atlasCanvas = Canvas(atlas)
        atlasCanvas.drawColor(0, PorterDuff.Mode.CLEAR)
        for (layer in layers) {
            atlasCanvas.drawBitmap(
                layer.bitmap, layer.srcX.toFloat(), layer.srcY.toFloat(), null
            )
            layer.bitmap.recycle()
        }

        var out = 1
        for (layer in layers) {
            if (out + 9 > layerOut.size)
                throw IllegalStateException("layerOut too small")
            layerOut[out++] = layer.element
            layerOut[out++] = layer.slot
            layerOut[out++] = layer.srcX
            layerOut[out++] = layer.srcY
            layerOut[out++] = layer.width
            layerOut[out++] = layer.height
            layerOut[out++] = layer.dstX
            layerOut[out++] = layer.dstY
            layerOut[out++] = 0
        }
        layerOut[0] = layers.size
        return atlas
    }

    private fun newLayer(
        element: Int,
        slot: Int,
        width: Int,
        height: Int,
        dstX: Int,
        dstY: Int
    ): Layer {
        return Layer(
            element, slot, width, height, dstX, dstY,
            Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        )
    }

    private fun addButtonLayers(
        layers: MutableList<Layer>,
        element: Int,
        box: Rect,
        snappingSize: Int,
        scale: Float,
        shape: Int,
        cornerRadius: Float,
        strokeWidth: Float,
        strokePad: Int,
        text: String,
        icon: Bitmap?
    ) {
        val halfStroke = strokeWidth * 0.5f
        val radius = cornerRadius * snappingSize * scale
        val width = box.width() + strokePad * 2
        val height = box.height() + strokePad * 2
        if (width <= 0 || height <= 0) return

        val fill = newLayer(
            element, SLOT_FILL, width, height,
            box.left - strokePad, box.top - strokePad
        )
        var canvas = Canvas(fill.bitmap)
        canvas.translate(
            -box.left.toFloat() + strokePad, -box.top.toFloat() + strokePad
        )
        var paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.style = Paint.Style.FILL
        paint.color = Color.WHITE
        drawButtonShape(canvas, box, shape, radius, halfStroke, paint, true)
        if (icon != null) {
            drawIcon(canvas, icon, box, snappingSize, scale, shape, true)
        } else if (text.isNotEmpty()) {
            paint.textSize = textSize(
                paint, text, box.width() - strokeWidth * 2.0f,
                snappingSize * 2.0f * scale
            )
            paint.style = Paint.Style.FILL
            drawCenteredTextXFER(
                canvas, paint, text, box.centerX().toFloat(),
                box.centerY().toFloat(), XFER_CLEAR
            )
        }
        layers.add(fill)

        val stroke = newLayer(
            element, SLOT_STROKE_TEXT, width, height,
            box.left - strokePad, box.top - strokePad
        )
        canvas = Canvas(stroke.bitmap)
        canvas.translate(
            -box.left.toFloat() + strokePad, -box.top.toFloat() + strokePad
        )
        paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = strokeWidth
        paint.color = Color.WHITE
        drawButtonShape(canvas, box, shape, radius, 0.0f, paint, false)
        if (icon != null) {
            drawIcon(canvas, icon, box, snappingSize, scale, shape, false)
        } else if (text.isNotEmpty()) {
            paint.style = Paint.Style.FILL
            paint.color = Color.WHITE
            paint.textSize = textSize(
                paint, text, box.width() - strokeWidth * 2.0f,
                snappingSize * 2.0f * scale
            )
            drawCenteredText(
                canvas, paint, text, box.centerX().toFloat(),
                box.centerY().toFloat()
            )
        }
        layers.add(stroke)

        val glow = makeGlowLayer(
            element, box, snappingSize, shape, radius, strokeWidth, strokePad
        )
        if (glow != null) layers.add(glow)

        val lpsWidth = strokeWidth
        val lpsOff = strokeWidth * 0.5f + snappingSize * 0.1f + lpsWidth * 0.5f
        val outerPad = ceil(lpsOff + lpsWidth).toInt() + 4
        val outerW = box.width() + outerPad * 2
        val outerH = box.height() + outerPad * 2
        if (outerW > 0 && outerH > 0) {
            val outer = newLayer(
                element, SLOT_OUTER, outerW, outerH,
                box.left - outerPad, box.top - outerPad
            )
            canvas = Canvas(outer.bitmap)
            canvas.translate(
                -box.left.toFloat() + outerPad, -box.top.toFloat() + outerPad
            )
            paint = Paint(Paint.ANTI_ALIAS_FLAG)
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = lpsWidth
            paint.color = Color.WHITE
            if (shape == SHAPE_CIRCLE) {
                canvas.drawCircle(
                    box.centerX().toFloat(), box.centerY().toFloat(),
                    box.width() * 0.5f + lpsOff, paint
                )
            } else {
                val outerRadius = radius + lpsOff
                canvas.drawRoundRect(
                    box.left - lpsOff, box.top - lpsOff,
                    box.right + lpsOff, box.bottom + lpsOff,
                    outerRadius, outerRadius, paint
                )
            }
            layers.add(outer)
        }
    }

    private fun makeGlowLayer(
        element: Int,
        box: Rect,
        snappingSize: Int,
        shape: Int,
        radius: Float,
        strokeWidth: Float,
        strokePad: Int
    ): Layer? {
        val halfStroke = strokeWidth * 0.5f
        val glowRadius = 3.0f * snappingSize
        val blurPad = ceil(glowRadius).toInt() + 4
        val glowPad = strokePad + ceil(glowRadius).toInt() + blurPad
        val width = box.width() + glowPad * 2
        val height = box.height() + glowPad * 2
        if (width <= 0 || height <= 0) return null

        val layer = newLayer(
            element, SLOT_GLOW, width, height,
            box.left - glowPad, box.top - glowPad
        )
        val canvas = Canvas(layer.bitmap)
        canvas.translate(
            -box.left.toFloat() + glowPad, -box.top.toFloat() + glowPad
        )

        val fullPath = Path()
        fullPath.addRect(
            (box.left - glowPad).toFloat(), (box.top - glowPad).toFloat(),
            (box.right + glowPad).toFloat(), (box.bottom + glowPad).toFloat(),
            Path.Direction.CW
        )
        val buttonPath = Path()
        if (shape == SHAPE_CIRCLE) {
            buttonPath.addCircle(
                box.centerX().toFloat(), box.centerY().toFloat(),
                box.width() * 0.5f + halfStroke, Path.Direction.CW
            )
        } else if (radius > 0) {
            buttonPath.addRoundRect(
                box.left - halfStroke, box.top - halfStroke,
                box.right + halfStroke, box.bottom + halfStroke,
                radius, radius, Path.Direction.CW
            )
        } else {
            buttonPath.addRect(
                box.left - halfStroke, box.top - halfStroke,
                box.right + halfStroke, box.bottom + halfStroke,
                Path.Direction.CW
            )
        }
        fullPath.op(buttonPath, Path.Op.DIFFERENCE)
        canvas.clipPath(fullPath)

        val expand = glowRadius * 0.3f
        val glowFill = Path()
        if (shape == SHAPE_CIRCLE) {
            glowFill.addCircle(
                box.centerX().toFloat(), box.centerY().toFloat(),
                box.width() * 0.5f + halfStroke + expand,
                Path.Direction.CW
            )
        } else if (radius > 0) {
            val glowR = radius + expand
            glowFill.addRoundRect(
                box.left - halfStroke - expand,
                box.top - halfStroke - expand,
                box.right + halfStroke + expand,
                box.bottom + halfStroke + expand,
                glowR, glowR, Path.Direction.CW
            )
        } else {
            glowFill.addRect(
                box.left - halfStroke - expand,
                box.top - halfStroke - expand,
                box.right + halfStroke + expand,
                box.bottom + halfStroke + expand,
                Path.Direction.CW
            )
        }

        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(96, 255, 255, 255)
        paint.maskFilter = BlurMaskFilter(
            glowRadius * 0.3f, BlurMaskFilter.Blur.NORMAL
        )
        canvas.drawPath(glowFill, paint)
        paint.maskFilter = BlurMaskFilter(
            glowRadius, BlurMaskFilter.Blur.NORMAL
        )
        canvas.drawPath(glowFill, paint)
        paint.maskFilter = null
        return layer
    }

    private fun addDPadLayers(
        layers: MutableList<Layer>,
        element: Int,
        box: Rect,
        snappingSize: Int,
        scale: Float,
        cornerRadius: Float,
        strokeWidth: Float,
        strokePad: Int
    ) {
        val width = box.width() + strokePad * 2
        val height = box.height() + strokePad * 2
        if (width <= 0 || height <= 0) return

        val fill = newLayer(
            element, SLOT_FILL, width, height,
            box.left - strokePad, box.top - strokePad
        )
        var canvas = Canvas(fill.bitmap)
        canvas.translate(
            -box.left.toFloat() + strokePad, -box.top.toFloat() + strokePad
        )
        var petal = buildUpPetal(box, snappingSize, scale)
        var paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.style = Paint.Style.FILL
        paint.color = Color.WHITE
        canvas.drawPath(petal, paint)
        layers.add(fill)

        val active = newLayer(
            element, SLOT_DPAD_ACTIVE, width, height,
            box.left - strokePad, box.top - strokePad
        )
        canvas = Canvas(active.bitmap)
        canvas.translate(
            -box.left.toFloat() + strokePad, -box.top.toFloat() + strokePad
        )
        petal = buildUpPetal(box, snappingSize, scale)
        paint = Paint(Paint.ANTI_ALIAS_FLAG)
        applyPathEffect(paint, cornerRadius, snappingSize, scale)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = strokeWidth
        paint.color = Color.WHITE
        canvas.drawPath(petal, paint)
        paint.style = Paint.Style.FILL
        canvas.drawPath(petal, paint)
        layers.add(active)

        val inactive = newLayer(
            element, SLOT_DPAD_INACTIVE, width, height,
            box.left - strokePad, box.top - strokePad
        )
        canvas = Canvas(inactive.bitmap)
        canvas.translate(
            -box.left.toFloat() + strokePad, -box.top.toFloat() + strokePad
        )
        petal = buildUpPetal(box, snappingSize, scale)
        paint = Paint(Paint.ANTI_ALIAS_FLAG)
        applyPathEffect(paint, cornerRadius, snappingSize, scale)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = strokeWidth
        paint.color = Color.WHITE
        canvas.drawPath(petal, paint)
        layers.add(inactive)
    }

    private fun applyPathEffect(
        paint: Paint,
        cornerRadius: Float,
        snappingSize: Int,
        scale: Float
    ) {
        val effective = cornerRadius * snappingSize * scale
        if (effective > 0)
            paint.pathEffect = CornerPathEffect(effective)
    }

    private fun buildUpPetal(
        box: Rect,
        snappingSize: Int,
        scale: Float
    ): Path {
        val cx = box.centerX().toFloat()
        val cy = box.centerY().toFloat()
        val offsetX = snappingSize * 2.0f * scale
        val offsetY = snappingSize * 3.0f * scale
        val start = snappingSize * scale
        val path = Path()
        path.moveTo(cx, cy - start)
        path.lineTo(cx - offsetX, cy - offsetY)
        path.lineTo(cx - offsetX, box.top.toFloat())
        path.lineTo(cx + offsetX, box.top.toFloat())
        path.lineTo(cx + offsetX, cy - offsetY)
        path.close()
        return path
    }

    private fun drawButtonShape(
        canvas: Canvas,
        box: Rect,
        shape: Int,
        radius: Float,
        halfStroke: Float,
        paint: Paint,
        expand: Boolean
    ) {
        if (shape == SHAPE_CIRCLE) {
            canvas.drawCircle(
                box.centerX().toFloat(), box.centerY().toFloat(),
                box.width() * 0.5f + if (expand) halfStroke else 0.0f,
                paint
            )
            return
        }
        if (expand) {
            val r = radius + halfStroke
            if (r > 0) {
                canvas.drawRoundRect(
                    box.left - halfStroke, box.top - halfStroke,
                    box.right + halfStroke, box.bottom + halfStroke,
                    r, r, paint
                )
            } else {
                canvas.drawRect(
                    box.left - halfStroke, box.top - halfStroke,
                    box.right + halfStroke, box.bottom + halfStroke,
                    paint
                )
            }
        } else if (radius > 0) {
            canvas.drawRoundRect(
                box.left.toFloat(), box.top.toFloat(),
                box.right.toFloat(), box.bottom.toFloat(),
                radius, radius, paint
            )
        } else {
            canvas.drawRect(
                box.left.toFloat(), box.top.toFloat(),
                box.right.toFloat(), box.bottom.toFloat(), paint
            )
        }
    }

    private fun textSize(
        paint: Paint,
        text: String,
        desiredWidth: Float,
        maxTextSize: Float
    ): Float {
        paint.textSize = 48.0f
        val measured = paint.measureText(text)
        if (measured <= 0.0f) return maxOf(1.0f, maxTextSize)
        val size = 48.0f * desiredWidth / measured
        return minOf(size, maxTextSize)
    }

    private fun drawCenteredText(
        canvas: Canvas,
        paint: Paint,
        text: String,
        centerX: Float,
        centerY: Float
    ) {
        paint.textAlign = Paint.Align.CENTER
        canvas.drawText(
            text, centerX,
            centerY - (paint.descent() + paint.ascent()) * 0.5f, paint
        )
    }

    private fun drawCenteredTextXFER(
        canvas: Canvas,
        paint: Paint,
        text: String,
        centerX: Float,
        centerY: Float,
        mode: Xfermode
    ) {
        paint.textAlign = Paint.Align.CENTER
        paint.xfermode = mode
        canvas.drawText(
            text, centerX,
            centerY - (paint.descent() + paint.ascent()) * 0.5f, paint
        )
        paint.xfermode = null
    }

    private fun loadIcon(context: Context, iconId: Int): Bitmap? {
        val appContext = context.applicationContext ?: context
        return try {
            appContext.assets.open("inputcontrols/icons/$iconId.png").use {
                BitmapFactory.decodeStream(it)
            }
        } catch (e: IOException) {
            null
        }
    }

    private fun drawIcon(
        canvas: Canvas,
        icon: Bitmap,
        box: Rect,
        snappingSize: Int,
        scale: Float,
        shape: Int,
        clear: Boolean
    ) {
        val margin = (snappingSize *
            (if (shape == SHAPE_CIRCLE) 2.0f else 1.0f) * scale).toInt()
        val halfSize = (minOf(box.width(), box.height()) - margin) / 2
        if (halfSize <= 0) return

        val src = Rect(0, 0, icon.width, icon.height)
        val dst = Rect(
            box.centerX() - halfSize, box.centerY() - halfSize,
            box.centerX() + halfSize, box.centerY() + halfSize
        )
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.isFilterBitmap = true
        if (clear) {
            paint.xfermode = XFER_DST_OUT
        } else {
            paint.colorFilter = PorterDuffColorFilter(
                Color.WHITE, PorterDuff.Mode.SRC_IN
            )
        }
        canvas.drawBitmap(icon, src, dst, paint)
    }
}
