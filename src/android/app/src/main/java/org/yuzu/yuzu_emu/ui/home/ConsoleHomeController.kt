// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.animation.ArgbEvaluator
import android.animation.ValueAnimator
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.view.Gravity
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.TextView
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.navigation.fragment.findNavController
import androidx.recyclerview.widget.RecyclerView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.GameAdapter
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.ui.CarouselRecyclerView
import org.yuzu.yuzu_emu.ui.main.MainActivity
import org.yuzu.yuzu_emu.utils.GameIconUtils
import org.yuzu.yuzu_emu.utils.MemoryUtil
import java.util.Locale

/**
 * Presentation layer of the console-style Home (portrait and landscape).
 *
 * It only *decorates* the existing games screen: GamesFragment, GameAdapter, GamesViewModel and the
 * launch flow are reused untouched. This class:
 *  - switches the carousel into its flat console style,
 *  - follows the centered game and shows its details (title, version, developer, playtime,
 *    format and file size),
 *  - tints the background with the dominant color of the selected game's icon,
 *  - provides the gear menu (settings, search, view, sort, install, multiplayer, applets, about),
 *  - adds gamepad shortcuts (X = game options, Y = search).
 *
 * Nothing here runs continuously: work happens only on selection changes.
 */
class ConsoleHomeController private constructor(
    private val fragment: Fragment,
    private val root: View,
    private val carousel: CarouselRecyclerView,
    private val adapter: GameAdapter,
    private val actions: Actions
) {
    /** Existing GamesFragment behaviour reused by the gear menu. */
    class Actions(
        val openSettings: () -> Unit,
        val focusSearch: () -> Unit,
        val showViewMenu: (anchor: View) -> Unit,
        val showSortMenu: (anchor: View) -> Unit
    )

    private val details: View = root.findViewById(R.id.console_details)
    private val gameTitle: TextView = root.findViewById(R.id.console_game_title)
    private val gameVersion: TextView = root.findViewById(R.id.console_game_version)
    private val metaDeveloper: TextView = root.findViewById(R.id.console_meta_developer)
    private val metaPlaytime: TextView = root.findViewById(R.id.console_meta_playtime)
    private val metaFormat: TextView = root.findViewById(R.id.console_meta_format)
    private val metaSize: TextView = root.findViewById(R.id.console_meta_size)
    private val metaContainer: LinearLayout = root.findViewById(R.id.console_meta_container)
    private val menuButton: View = root.findViewById(R.id.settings_button)
    private val headerSpace: View = root.findViewById(R.id.console_header_space)
    private val searchFrame: View = root.findViewById(R.id.frame_search)
    private val searchText: EditText = root.findViewById(R.id.search_text)

    private val res = root.resources
    private val detailsFadeMs = res.getInteger(R.integer.console_home_details_fade_ms).toLong()
    private val detailsShiftPx = res.getDimension(R.dimen.console_home_details_shift)

    private var active = false
    private var released = false
    private var selectedGame: Game? = null
    private var pendingPosition = RecyclerView.NO_POSITION
    private var sizeJob: Job? = null
    private var menu: PopupWindow? = null
    private var colorJob: Job? = null
    private var colorAnimator: ValueAnimator? = null

    // Background gradient (top, center, bottom), tinted from the selected game's icon.
    private val defaultBackground = intArrayOf(
        res.getColor(R.color.console_home_bg_top, null),
        res.getColor(R.color.console_home_bg_center, null),
        res.getColor(R.color.console_home_bg_bottom, null)
    )
    private val background = GradientDrawable(
        GradientDrawable.Orientation.TOP_BOTTOM,
        defaultBackground.clone()
    )
    private var backgroundColors = defaultBackground.clone()

    private val applySelectionRunnable = Runnable { applySelection() }

    companion object {
        // File sizes by game path, kept for the process lifetime (querying the provider is slow).
        private val sizeCache = HashMap<String, Long>()

        // Dominant icon color by game path, kept for the process lifetime.
        private val colorCache = HashMap<String, Int>()

        private const val BACKGROUND_FADE_MS = 350L
        private const val HUE_BUCKETS = 12
        private const val SAMPLE_SIZE = 24

        /**
         * Attaches the console Home if the current layout provides it (res/layout/fragment_games.xml).
         * Returns null for layouts without console views, leaving behaviour unchanged.
         */
        fun attachIfSupported(
            fragment: Fragment,
            root: View,
            carousel: CarouselRecyclerView?,
            adapter: GameAdapter,
            actions: Actions
        ): ConsoleHomeController? {
            if (carousel == null || root.findViewById<View?>(R.id.console_details) == null) {
                return null
            }
            return ConsoleHomeController(fragment, root, carousel, adapter, actions).also { it.bind() }
        }
    }

    private fun bind() {
        carousel.onCenteredPositionChanged = { position, _ -> onCenteredPosition(position) }
        carousel.keyInterceptor = ::handleCarouselKey

        root.background = background

        // Landscape: details in a single row so the carousel keeps the portrait tile size.
        if (res.getBoolean(R.bool.console_home_meta_horizontal)) {
            metaContainer.orientation = LinearLayout.HORIZONTAL
            val gap = res.getDimensionPixelSize(R.dimen.console_home_meta_gap)
            for (i in 0 until metaContainer.childCount) {
                val params = metaContainer.getChildAt(i).layoutParams as LinearLayout.LayoutParams
                params.topMargin = 0
                params.marginEnd = gap
            }
        }

        root.findViewById<View?>(R.id.console_profile_button)?.setOnClickListener {
            navigateToSubscreen(SettingsSubscreen.PROFILE_MANAGER)
        }

        // The search field only takes room in the top bar while it is in use.
        searchText.setOnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) hideSearchIfEmpty()
        }
        if (searchText.text.isNotEmpty()) showSearch(focus = false)
    }

    /**
     * Enables the console presentation only while the carousel view type is in use. With grid/list
     * views selected from the "view" menu the details panel is hidden and the screen behaves as
     * before.
     */
    fun setActive(isCarousel: Boolean) {
        if (released) return
        active = isCarousel
        carousel.consoleStyle = isCarousel
        if (!isCarousel) {
            clearSelection()
            details.visibility = View.GONE
            animateBackground(defaultBackground)
        } else if (details.visibility == View.GONE) {
            details.visibility = View.INVISIBLE
        }
    }

    /** Called by GamesFragment after every list submission (filter, search, reload). */
    fun onListSubmitted(isEmpty: Boolean) {
        if (released || !active) return
        if (isEmpty) {
            clearSelection()
        } else {
            // Positions may now point to different games; re-evaluate on the next frame.
            selectedGame = null
            // Note: not refreshView(), which would also steal focus from the search field.
            carousel.post { if (!released) carousel.updateChildScalesAndAlpha() }
        }
    }

    fun onResume() {
        if (released) return
        // Playtime changes after playing: refresh the details of the selected game.
        if (active) {
            selectedGame = null
            carousel.post { if (!released) carousel.updateChildScalesAndAlpha() }
        }
    }

    fun release() {
        released = true
        sizeJob?.cancel()
        sizeJob = null
        colorJob?.cancel()
        colorJob = null
        colorAnimator?.cancel()
        colorAnimator = null
        menu?.dismiss()
        menu = null
        details.removeCallbacks(applySelectionRunnable)
        details.animate().cancel()
        carousel.onCenteredPositionChanged = null
        carousel.keyInterceptor = null
        searchText.onFocusChangeListener = null
    }

    // ---------------------------------------------------------------------------------------------
    // Selection & details

    private fun onCenteredPosition(position: Int) {
        if (!active || released) return
        pendingPosition = position
        // Coalesce: the carousel reports on every scroll frame / layout pass; touching text views
        // inside a layout pass would trigger another layout, so apply on the next frame.
        details.removeCallbacks(applySelectionRunnable)
        details.post(applySelectionRunnable)
    }

    private fun applySelection() {
        if (!active || released) return
        val game = adapter.currentList.getOrNull(pendingPosition) ?: return
        if (game == selectedGame) return
        selectedGame = game

        gameTitle.text = game.title.replace("[\\t\\n\\r]+".toRegex(), " ")
        val version = game.version.trim()
        gameVersion.text = if (version.isNotEmpty()) "v$version" else ""
        gameVersion.visibility = if (version.isNotEmpty()) View.VISIBLE else View.GONE

        val developer = game.developer.trim()
        metaDeveloper.text = res.getString(R.string.home_console_meta_developer, developer)
        metaDeveloper.visibility = if (developer.isNotEmpty()) View.VISIBLE else View.GONE

        metaPlaytime.text = res.getString(
            R.string.home_console_meta_playtime,
            formatPlayTime(readPlayTime(game))
        )
        metaFormat.text = res.getString(R.string.home_console_meta_format, formatOf(game))
        bindSize(game)
        bindBackground(game)

        details.visibility = View.VISIBLE
        details.animate().cancel()
        details.alpha = 0.25f
        details.translationY = detailsShiftPx
        details.animate()
            .alpha(1f)
            .translationY(0f)
            .setDuration(detailsFadeMs)
            .start()
    }

    private fun clearSelection() {
        selectedGame = null
        pendingPosition = RecyclerView.NO_POSITION
        sizeJob?.cancel()
        colorJob?.cancel()
        details.removeCallbacks(applySelectionRunnable)
        details.animate().cancel()
        details.visibility = View.INVISIBLE
    }

    private fun readPlayTime(game: Game): Long = try {
        if (game.programId.isNotEmpty()) NativeLibrary.playTimeManagerGetPlayTime(game.programId) else 0L
    } catch (e: Exception) {
        0L
    }

    private fun formatPlayTime(seconds: Long): String {
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        val h = res.getString(R.string.hours_abbr)
        val m = res.getString(R.string.minutes_abbr)
        return if (hours > 0) "$hours$h $minutes$m" else "$minutes$m"
    }

    private fun formatOf(game: Game): String {
        val extension = Uri.decode(game.path).substringAfterLast('.', "").lowercase(Locale.ROOT)
        val format = if (extension in Game.extensions) extension.uppercase(Locale.ROOT) else "-"
        return if (game.isHomebrew) "$format · ${res.getString(R.string.home_console_homebrew)}" else format
    }

    /** File size from the existing NativeLibrary.getSize() (document provider / native path). */
    private fun bindSize(game: Game) {
        sizeJob?.cancel()
        val cached = sizeCache[game.path]
        if (cached != null) {
            showSize(cached)
            return
        }
        metaSize.visibility = View.INVISIBLE
        sizeJob = fragment.viewLifecycleOwner.lifecycleScope.launch {
            val bytes = withContext(Dispatchers.IO) {
                try {
                    NativeLibrary.getSize(game.path)
                } catch (e: Exception) {
                    0L
                }
            }
            sizeCache[game.path] = bytes
            if (!released && selectedGame == game) showSize(bytes)
        }
    }

    private fun showSize(bytes: Long) {
        if (bytes <= 0L) {
            metaSize.visibility = View.GONE
            return
        }
        metaSize.text = res.getString(
            R.string.home_console_meta_size,
            MemoryUtil.bytesToSizeUnit(bytes.toFloat())
        )
        metaSize.visibility = View.VISIBLE
    }

    // ---------------------------------------------------------------------------------------------
    // Dynamic background

    private fun bindBackground(game: Game) {
        colorJob?.cancel()
        val cached = colorCache[game.path]
        if (cached != null) {
            animateBackground(gradientFor(cached))
            return
        }
        colorJob = fragment.viewLifecycleOwner.lifecycleScope.launch {
            val color = try {
                val icon = GameIconUtils.getGameIcon(fragment.viewLifecycleOwner, game)
                withContext(Dispatchers.Default) { dominantColor(icon) }
            } catch (e: Exception) {
                null
            } ?: return@launch
            colorCache[game.path] = color
            if (!released && active && selectedGame == game) animateBackground(gradientFor(color))
        }
    }

    /**
     * Dominant color of the icon: pixels are grouped by hue (weighted by saturation and brightness)
     * and the heaviest group is averaged. Mostly grey icons fall back to the plain average.
     */
    private fun dominantColor(icon: Bitmap): Int {
        val sample = Bitmap.createScaledBitmap(icon, SAMPLE_SIZE, SAMPLE_SIZE, true)
        val pixels = IntArray(SAMPLE_SIZE * SAMPLE_SIZE)
        sample.getPixels(pixels, 0, SAMPLE_SIZE, 0, 0, SAMPLE_SIZE, SAMPLE_SIZE)
        if (sample !== icon) sample.recycle()

        val weights = FloatArray(HUE_BUCKETS)
        val sums = Array(HUE_BUCKETS) { FloatArray(3) }
        val total = FloatArray(3)
        val hsv = FloatArray(3)
        var count = 0
        for (pixel in pixels) {
            if (Color.alpha(pixel) < 128) continue
            val r = Color.red(pixel).toFloat()
            val g = Color.green(pixel).toFloat()
            val b = Color.blue(pixel).toFloat()
            total[0] += r
            total[1] += g
            total[2] += b
            count++
            Color.colorToHSV(pixel, hsv)
            val weight = hsv[1] * hsv[2]
            if (weight < 0.08f) continue
            val bucket = ((hsv[0] / 360f) * HUE_BUCKETS).toInt().coerceIn(0, HUE_BUCKETS - 1)
            weights[bucket] += weight
            sums[bucket][0] += r * weight
            sums[bucket][1] += g * weight
            sums[bucket][2] += b * weight
        }
        if (count == 0) return defaultBackground[1]

        val best = weights.indices.maxByOrNull { weights[it] } ?: 0
        val weight = weights[best]
        return if (weight >= count * 0.03f) {
            Color.rgb(
                (sums[best][0] / weight).toInt(),
                (sums[best][1] / weight).toInt(),
                (sums[best][2] / weight).toInt()
            )
        } else {
            Color.rgb(
                (total[0] / count).toInt(),
                (total[1] / count).toInt(),
                (total[2] / count).toInt()
            )
        }
    }

    /** Dark-to-light gradient in the hue of [color], dark enough to keep white text readable. */
    private fun gradientFor(color: Int): IntArray {
        val hsv = FloatArray(3)
        Color.colorToHSV(color, hsv)
        val hue = hsv[0]
        val saturation = hsv[1]
        return intArrayOf(
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.9f).coerceAtMost(0.85f), 0.16f)),
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.8f).coerceAtMost(0.75f), 0.30f)),
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.5f).coerceAtMost(0.5f), 0.50f))
        )
    }

    private fun animateBackground(target: IntArray) {
        colorAnimator?.cancel()
        if (released || target.contentEquals(backgroundColors)) return
        val start = backgroundColors.clone()
        val evaluator = ArgbEvaluator()
        colorAnimator = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = BACKGROUND_FADE_MS
            addUpdateListener { animator ->
                val fraction = animator.animatedFraction
                val colors = IntArray(target.size) { i ->
                    evaluator.evaluate(fraction, start[i], target[i]) as Int
                }
                backgroundColors = colors
                background.colors = colors
            }
            start()
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Gear menu

    private class MenuEntry(val iconId: Int, val labelId: Int, val action: (View) -> Unit)

    fun showMenu(anchor: View) {
        if (released) return
        menu?.dismiss()
        val context = anchor.context
        val inflater = LayoutInflater.from(context)
        val density = res.displayMetrics.density

        val entries = listOf(
            MenuEntry(R.drawable.ic_settings, R.string.preferences_settings) { actions.openSettings() },
            MenuEntry(R.drawable.ic_search, R.string.home_console_search) { showSearch(focus = true) },
            MenuEntry(R.drawable.ic_eye, R.string.home_console_view) { actions.showViewMenu(it) },
            MenuEntry(R.drawable.ic_filter, R.string.home_console_sort) { actions.showSortMenu(it) },
            MenuEntry(R.drawable.ic_install, R.string.install) {
                navigateToSubscreen(SettingsSubscreen.INSTALLABLE)
            },
            MenuEntry(R.drawable.ic_two_users, R.string.multiplayer) {
                (fragment.activity as? MainActivity)?.displayMultiplayerDialog()
            },
            MenuEntry(R.drawable.ic_applet, R.string.home_console_applets) { openApplets() },
            MenuEntry(R.drawable.ic_info_outline, R.string.about) {
                navigateToSubscreen(SettingsSubscreen.ABOUT)
            }
        )

        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundResource(R.drawable.console_menu_background)
            val pad = (8 * density).toInt()
            setPadding(pad, pad, pad, pad)
            minimumWidth = (240 * density).toInt()
        }
        val popup = PopupWindow(
            content,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            true
        ).apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            elevation = 12 * density
            setOnDismissListener { if (menu === this) menu = null }
        }

        var first: View? = null
        for (entry in entries) {
            val item = inflater.inflate(R.layout.item_console_menu, content, false)
            item.findViewById<ImageView>(R.id.console_menu_icon).setImageResource(entry.iconId)
            item.findViewById<TextView>(R.id.console_menu_label).setText(entry.labelId)
            item.setOnClickListener {
                popup.dismiss()
                entry.action(anchor)
            }
            content.addView(item)
            if (first == null) first = item
        }

        menu = popup
        popup.showAsDropDown(anchor, 0, (8 * density).toInt(), Gravity.START)
        // Gamepad / keyboard: start on the first entry. Touch mode is unaffected.
        first?.requestFocus()
    }

    private fun openApplets() {
        if (NativeLibrary.isFirmwareAvailable()) {
            navigateToSubscreen(SettingsSubscreen.APPLET_LAUNCHER)
        } else {
            Toast.makeText(
                fragment.requireContext(),
                R.string.applets_error_firmware,
                Toast.LENGTH_SHORT
            ).show()
        }
    }

    private fun navigateToSubscreen(destination: SettingsSubscreen) {
        val action = HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(destination, null)
        fragment.findNavController().navigate(action)
    }

    // ---------------------------------------------------------------------------------------------
    // Search

    private fun showSearch(focus: Boolean) {
        headerSpace.visibility = View.GONE
        searchFrame.visibility = View.VISIBLE
        if (focus) actions.focusSearch()
    }

    private fun hideSearchIfEmpty() {
        if (released || searchText.text.isNotEmpty()) return
        searchFrame.visibility = View.GONE
        headerSpace.visibility = View.VISIBLE
    }

    // ---------------------------------------------------------------------------------------------
    // Actions (reuse the existing GameAdapter click handlers so launch checks stay identical)

    private fun centeredItemView(): View? {
        val position = carousel.getClosestChildPosition()
        if (position == RecyclerView.NO_POSITION) return null
        return carousel.findViewHolderForAdapterPosition(position)?.itemView
    }

    /** Long click on a game card opens its properties screen. */
    private fun openOptionsForSelected() {
        centeredItemView()?.performLongClick()
    }

    private fun handleCarouselKey(event: KeyEvent): Boolean {
        if (!active) return false
        return when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_X -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    openOptionsForSelected()
                }
                true
            }
            KeyEvent.KEYCODE_BUTTON_Y -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    showSearch(focus = true)
                }
                true
            }
            else -> false
        }
    }
}
