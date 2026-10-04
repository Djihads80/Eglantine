// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.views

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.util.AttributeSet
import android.view.View
import androidx.core.content.ContextCompat
import org.yuzu.yuzu_emu.R
import kotlin.math.max

/**
 * Console-style status indicators (Wi-Fi + battery) for the Home and game screens.
 *
 * Values come from the system: battery from the sticky ACTION_BATTERY_CHANGED broadcast and
 * Wi-Fi from the default network callback (ACCESS_NETWORK_STATE / ACCESS_WIFI_STATE are already
 * declared in the manifest). Listeners are only registered while the view is attached.
 */
class ConsoleStatusView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private var batteryLevel = -1 // 0..100, -1 = unknown (e.g. no battery)
    private var isCharging = false
    private var wifiConnected = false
    private var wifiLevel = 0 // 0..WIFI_LEVELS-1

    private val density = resources.displayMetrics.density
    private val primaryColor = ContextCompat.getColor(context, R.color.console_home_text_primary)
    private val dimColor = ContextCompat.getColor(context, R.color.console_home_outline_dim)
    private val lowColor = ContextCompat.getColor(context, R.color.console_home_battery_low)

    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val rect = RectF()
    private val boltPath = Path()

    private val connectivityManager =
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
    private val wifiManager =
        context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
    private var networkCallbackRegistered = false
    private var batteryReceiverRegistered = false

    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            intent?.let { updateBattery(it) }
        }
    }

    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
            val connected = caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
            val level = if (connected) wifiLevelFrom(caps) else 0
            post { setWifiState(connected, level) }
        }

        override fun onLost(network: Network) {
            post { setWifiState(false, 0) }
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (isInEditMode) return
        // Sticky broadcast: returns the current state immediately.
        val sticky = ContextCompat.registerReceiver(
            context,
            batteryReceiver,
            IntentFilter(Intent.ACTION_BATTERY_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        batteryReceiverRegistered = true
        sticky?.let { updateBattery(it) }

        try {
            connectivityManager?.registerDefaultNetworkCallback(networkCallback)
            networkCallbackRegistered = connectivityManager != null
        } catch (e: Exception) {
            networkCallbackRegistered = false
        }
    }

    override fun onDetachedFromWindow() {
        if (batteryReceiverRegistered) {
            try {
                context.unregisterReceiver(batteryReceiver)
            } catch (_: IllegalArgumentException) {
            }
            batteryReceiverRegistered = false
        }
        if (networkCallbackRegistered) {
            try {
                connectivityManager?.unregisterNetworkCallback(networkCallback)
            } catch (_: IllegalArgumentException) {
            }
            networkCallbackRegistered = false
        }
        super.onDetachedFromWindow()
    }

    private fun updateBattery(intent: Intent) {
        val present = intent.getBooleanExtra(BatteryManager.EXTRA_PRESENT, true)
        val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        batteryLevel = if (present && level >= 0 && scale > 0) level * 100 / scale else -1
        isCharging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL
        updateDescription()
        invalidate()
    }

    private fun setWifiState(connected: Boolean, level: Int) {
        if (wifiConnected == connected && wifiLevel == level) return
        wifiConnected = connected
        wifiLevel = level
        updateDescription()
        invalidate()
    }

    @Suppress("DEPRECATION")
    private fun wifiLevelFrom(caps: NetworkCapabilities): Int {
        val rssi = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            caps.signalStrength
        } else {
            wifiManager?.connectionInfo?.rssi ?: NetworkCapabilities.SIGNAL_STRENGTH_UNSPECIFIED
        }
        if (rssi == NetworkCapabilities.SIGNAL_STRENGTH_UNSPECIFIED) return WIFI_LEVELS - 1
        return WifiManager.calculateSignalLevel(rssi, WIFI_LEVELS).coerceIn(0, WIFI_LEVELS - 1)
    }

    private fun updateDescription() {
        val wifi = if (wifiConnected) {
            context.getString(R.string.console_status_wifi_on)
        } else {
            context.getString(R.string.console_status_wifi_off)
        }
        contentDescription = if (batteryLevel >= 0) {
            "$wifi, " + context.getString(R.string.console_status_battery, batteryLevel)
        } else {
            wifi
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val h = (ICON_HEIGHT_DP * density).toInt() + paddingTop + paddingBottom
        val w = ((WIFI_WIDTH_DP + SPACING_DP + BATTERY_WIDTH_DP) * density).toInt() +
            paddingLeft + paddingRight
        setMeasuredDimension(resolveSize(w, widthMeasureSpec), resolveSize(h, heightMeasureSpec))
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val top = paddingTop.toFloat()
        val iconH = (height - paddingTop - paddingBottom).toFloat()
        var x = paddingLeft.toFloat()
        drawWifi(canvas, x, top, WIFI_WIDTH_DP * density, iconH)
        x += (WIFI_WIDTH_DP + SPACING_DP) * density
        if (batteryLevel >= 0) {
            drawBattery(canvas, x, top, BATTERY_WIDTH_DP * density, iconH)
        }
    }

    private fun drawWifi(canvas: Canvas, left: Float, top: Float, w: Float, h: Float) {
        val stroke = max(1.5f * density, h * 0.11f)
        strokePaint.strokeWidth = stroke
        val cx = left + w / 2f
        val cy = top + h - stroke
        // Dot + 3 arcs; arcs above the current level are dimmed.
        for (i in 1 until WIFI_LEVELS) {
            val radius = (h - stroke * 1.5f) * i / (WIFI_LEVELS - 1)
            strokePaint.color = if (wifiConnected && wifiLevel >= i) primaryColor else dimColor
            rect.set(cx - radius, cy - radius, cx + radius, cy + radius)
            canvas.drawArc(rect, 225f, 90f, false, strokePaint)
        }
        fillPaint.color = if (wifiConnected) primaryColor else dimColor
        canvas.drawCircle(cx, cy - stroke * 0.2f, stroke * 0.85f, fillPaint)
        if (!wifiConnected) {
            strokePaint.color = primaryColor
            canvas.drawLine(left + stroke, top + stroke, left + w - stroke, cy, strokePaint)
        }
    }

    private fun drawBattery(canvas: Canvas, left: Float, top: Float, w: Float, h: Float) {
        val stroke = max(1.2f * density, h * 0.08f)
        val bodyH = h * 0.62f
        val bodyTop = top + (h - bodyH) / 2f
        val tipW = w * 0.08f
        val bodyW = w - tipW - stroke
        val radius = bodyH * 0.22f

        strokePaint.strokeWidth = stroke
        strokePaint.color = primaryColor
        rect.set(left + stroke / 2f, bodyTop, left + bodyW, bodyTop + bodyH)
        canvas.drawRoundRect(rect, radius, radius, strokePaint)

        fillPaint.color = primaryColor
        val tipH = bodyH * 0.4f
        rect.set(left + bodyW + stroke * 0.6f, bodyTop + (bodyH - tipH) / 2f,
            left + bodyW + stroke * 0.6f + tipW, bodyTop + (bodyH + tipH) / 2f)
        canvas.drawRoundRect(rect, tipW / 2f, tipW / 2f, fillPaint)

        val inset = stroke * 1.6f
        val fillMax = bodyW - inset * 2f + stroke / 2f
        val fillW = fillMax * batteryLevel.coerceIn(0, 100) / 100f
        fillPaint.color = if (!isCharging && batteryLevel <= LOW_BATTERY) lowColor else primaryColor
        rect.set(left + inset, bodyTop + inset, left + inset + fillW, bodyTop + bodyH - inset)
        canvas.drawRoundRect(rect, radius / 2f, radius / 2f, fillPaint)

        if (isCharging) {
            // Small bolt over the body, drawn in the background color for contrast.
            val cx = left + bodyW / 2f
            val cy = bodyTop + bodyH / 2f
            val s = bodyH * 0.42f
            boltPath.reset()
            boltPath.moveTo(cx + s * 0.15f, cy - s)
            boltPath.lineTo(cx - s * 0.45f, cy + s * 0.1f)
            boltPath.lineTo(cx - s * 0.02f, cy + s * 0.1f)
            boltPath.lineTo(cx - s * 0.15f, cy + s)
            boltPath.lineTo(cx + s * 0.45f, cy - s * 0.1f)
            boltPath.lineTo(cx + s * 0.02f, cy - s * 0.1f)
            boltPath.close()
            fillPaint.color = ContextCompat.getColor(context, R.color.console_home_bg_top)
            canvas.drawPath(boltPath, fillPaint)
        }
    }

    private companion object {
        const val WIFI_LEVELS = 4
        const val LOW_BATTERY = 15
        const val ICON_HEIGHT_DP = 18f
        const val WIFI_WIDTH_DP = 22f
        const val BATTERY_WIDTH_DP = 34f
        const val SPACING_DP = 14f
    }
}
