// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
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
import androidx.core.widget.ImageViewCompat
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
import org.yuzu.yuzu_emu.views.ConsoleLiquidBackgroundView
import java.io.File

/**
 * Presentation layer of the console-style Home (portrait and landscape).
 *
 * It only *decorates* the existing games screen: GamesFragment, GameAdapter, GamesViewModel and the
 * launch flow are reused untouched. This class:
 *  - switches the carousel into its flat console style,
 *  - follows the centered game and shows its title,
 *  - tints the animated background (and the selected tile outline) with the main colors of the
 *    selected game's icon,
 *  - shows the current user's avatar on the profile button,
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
    private val liquidBackground: ConsoleLiquidBackgroundView =
        root.findViewById(R.id.console_background)
    private val profileImage: ImageView = root.findViewById(R.id.console_profile_image)
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
    private var menu: PopupWindow? = null
    private var colorJob: Job? = null
    private var avatarJob: Job? = null

    // Background gradient (top, center, bottom) and accents used when no game is selected.
    private val defaultBackground = intArrayOf(
        res.getColor(R.color.console_home_bg_top, null),
        res.getColor(R.color.console_home_bg_center, null),
        res.getColor(R.color.console_home_bg_bottom, null)
    )
    private val defaultAccents = intArrayOf(
        Color.rgb(0x3A, 0x5B, 0xD9),
        Color.rgb(0x6A, 0x4C, 0xC9),
        Color.rgb(0x2E, 0x8C, 0xC7)
    )

    private val applySelectionRunnable = Runnable { applySelection() }

    companion object {
        // Main icon colors (accents) by game path, kept for the process lifetime.
        private val colorCache = HashMap<String, IntArray>()

        private const val ACCENT_COUNT = 3
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

        liquidBackground.setPalette(defaultBackground, defaultAccents, animate = false)
        loadAvatar()

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
            liquidBackground.setPalette(defaultBackground, defaultAccents)
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
        // The user may have changed profile or avatar.
        loadAvatar()
        // Re-evaluate the selected game (title and background colors).
        if (active) {
            selectedGame = null
            carousel.post { if (!released) carousel.updateChildScalesAndAlpha() }
        }
    }

    fun release() {
        released = true
        colorJob?.cancel()
        colorJob = null
        avatarJob?.cancel()
        avatarJob = null
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
        colorJob?.cancel()
        details.removeCallbacks(applySelectionRunnable)
        details.animate().cancel()
        details.visibility = View.INVISIBLE
    }

    // ---------------------------------------------------------------------------------------------
    // Dynamic background

    private fun bindBackground(game: Game) {
        colorJob?.cancel()
        val cached = colorCache[game.path]
        if (cached != null) {
            applyAccents(cached)
            return
        }
        colorJob = fragment.viewLifecycleOwner.lifecycleScope.launch {
            val accents = try {
                val icon = GameIconUtils.getGameIcon(fragment.viewLifecycleOwner, game)
                withContext(Dispatchers.Default) { accentColors(icon) }
            } catch (e: Exception) {
                null
            } ?: return@launch
            colorCache[game.path] = accents
            if (!released && active && selectedGame == game) applyAccents(accents)
        }
    }

    private fun applyAccents(accents: IntArray) {
        val liquidAccents = IntArray(accents.size) { liquidAccent(accents[it]) }
        liquidBackground.setPalette(gradientFor(accents[0]), liquidAccents)
    }

    /**
     * Main colors of the icon: pixels are grouped by hue (weighted by saturation and brightness)
     * and the heaviest groups are averaged, strongest first. Missing slots (few colors, or a mostly
     * grey icon) are filled with hue shifts of the main color.
     */
    private fun accentColors(icon: Bitmap): IntArray {
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
        if (count == 0) return defaultAccents.clone()

        val minWeight = count * 0.03f
        val found = weights.indices
            .filter { weights[it] >= minWeight }
            .sortedByDescending { weights[it] }
            .take(ACCENT_COUNT)
            .map { bucket ->
                val weight = weights[bucket]
                Color.rgb(
                    (sums[bucket][0] / weight).toInt(),
                    (sums[bucket][1] / weight).toInt(),
                    (sums[bucket][2] / weight).toInt()
                )
            }
        val main = found.firstOrNull() ?: Color.rgb(
            (total[0] / count).toInt(),
            (total[1] / count).toInt(),
            (total[2] / count).toInt()
        )
        return IntArray(ACCENT_COUNT) { i ->
            found.getOrNull(i) ?: shiftHue(main, if (i % 2 == 1) 28f else -28f)
        }
    }

    private fun shiftHue(color: Int, degrees: Float): Int {
        val hsv = FloatArray(3)
        Color.colorToHSV(color, hsv)
        hsv[0] = (hsv[0] + degrees + 360f) % 360f
        return Color.HSVToColor(hsv)
    }

    /** Accent tuned for the liquid blobs: vivid but not glaring under white text. */
    private fun liquidAccent(color: Int): Int {
        val hsv = FloatArray(3)
        Color.colorToHSV(color, hsv)
        hsv[1] = hsv[1].coerceIn(0.35f, 0.9f)
        hsv[2] = hsv[2].coerceIn(0.45f, 0.8f)
        return Color.HSVToColor(hsv)
    }

    /** Dark-to-light gradient in the hue of [color], dark enough to keep white text readable. */
    private fun gradientFor(color: Int): IntArray {
        val hsv = FloatArray(3)
        Color.colorToHSV(color, hsv)
        val hue = hsv[0]
        val saturation = hsv[1]
        return intArrayOf(
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.9f).coerceAtMost(0.85f), 0.14f)),
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.8f).coerceAtMost(0.75f), 0.26f)),
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.5f).coerceAtMost(0.5f), 0.42f))
        )
    }

    // ---------------------------------------------------------------------------------------------
    // Profile avatar

    /** Shows the current user's avatar (same image as the profile manager) on its button. */
    private fun loadAvatar() {
        avatarJob?.cancel()
        avatarJob = fragment.viewLifecycleOwner.lifecycleScope.launch {
            val avatar = withContext(Dispatchers.IO) {
                try {
                    val uuid = NativeLibrary.getCurrentUser()
                    val path = uuid?.let { NativeLibrary.getUserImagePath(it) }
                    if (path != null && File(path).exists()) {
                        BitmapFactory.decodeFile(path)
                    } else {
                        val jpeg = NativeLibrary.getDefaultAccountBackupJpeg()
                        BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size)
                    }
                } catch (e: Exception) {
                    null
                }
            }
            if (released) return@launch
            if (avatar != null) {
                ImageViewCompat.setImageTintList(profileImage, null)
                profileImage.scaleType = ImageView.ScaleType.CENTER_CROP
                profileImage.layoutParams = profileImage.layoutParams.apply {
                    width = ViewGroup.LayoutParams.MATCH_PARENT
                    height = ViewGroup.LayoutParams.MATCH_PARENT
                }
                profileImage.setImageBitmap(avatar)
            }
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
