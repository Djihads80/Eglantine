// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.graphics.BitmapFactory
import android.animation.ValueAnimator
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Shader
import android.view.animation.LinearInterpolator
import kotlin.math.PI
import kotlin.math.cos
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
import androidx.activity.OnBackPressedCallback
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
import org.yuzu.yuzu_emu.views.ConsoleLiquidBackgroundView
import org.yuzu.yuzu_emu.views.ConsolePalette
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
 *  - shows the current user's avatar on the profile button and opens the profile manager in a
 *    console panel over Home,
 *  - provides the gear menu (settings, search, view, sort, install, multiplayer, applets, about),
 *  - adds gamepad shortcuts (X = play, Y = search).
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
    private val overlay: View = root.findViewById(R.id.console_overlay)
    private val overlayTitle: TextView = root.findViewById(R.id.console_overlay_title)
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
    private var titleGlow: ValueAnimator? = null

    // Profile manager shown over Home (instead of the settings activity).
    private val subscreenHost = ConsoleSubscreenHost(
        fragment,
        R.id.console_overlay_container,
        onTitle = { overlayTitle.text = it }
    ) { onOverlayClosed() }
    private val overlayBackCallback = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() {
            subscreenHost.back()
        }
    }

    // Background used when no game is selected.
    private val defaultPalette = ConsoleGameColors.defaultPalette(res)

    private val applySelectionRunnable = Runnable { applySelection() }

    companion object {
        private const val OVERLAY_FADE_MS = 180L
        private const val TITLE_GLOW_PERIOD_MS = 3600L
        private const val TITLE_SWEEP_FRACTION = 0.55f
        private const val TITLE_GLOW_MIN_DP = 4f
        private const val TITLE_GLOW_MAX_DP = 10f

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

        liquidBackground.setPalette(defaultPalette.base, defaultPalette.accents, animate = false)
        startTitleGlow()
        loadAvatar()

        root.findViewById<View?>(R.id.console_profile_button)?.setOnClickListener {
            openOverlay(SettingsSubscreen.PROFILE_MANAGER)
        }
        root.findViewById<View>(R.id.console_overlay_back).setOnClickListener {
            subscreenHost.back()
        }
        fragment.requireActivity().onBackPressedDispatcher.addCallback(
            fragment.viewLifecycleOwner,
            overlayBackCallback
        )

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
            liquidBackground.setPalette(defaultPalette.base, defaultPalette.accents)
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
        overlayBackCallback.remove()
        titleGlow?.cancel()
        titleGlow = null
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
    // Title glow

    private var shimmerShader: LinearGradient? = null
    private var shimmerKey = Long.MIN_VALUE
    private val shimmerMatrix = Matrix()

    /**
     * Lighting on the selected game's title, in the bright palette colors of the animated
     * background (same as the tile outline): a band of colored light sweeps across the letters,
     * with a soft halo breathing around them. The title has padding (negative margins keep its
     * position) so the halo is never cut into a box. Follows palette changes.
     */
    private fun startTitleGlow() {
        val density = res.displayMetrics.density
        titleGlow = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = TITLE_GLOW_PERIOD_MS
            repeatCount = ValueAnimator.INFINITE
            interpolator = LinearInterpolator()
            addUpdateListener { animator ->
                if (released || !gameTitle.isShown) return@addUpdateListener
                val t = animator.animatedValue as Float
                val colors = ConsolePalette.borderColors
                updateTitleShimmer(t, colors)

                // Halo: gentle breathing, peaking while the light crosses the title.
                val breath = (0.5f - 0.5f * cos(2f * PI.toFloat() * t))
                val radius = (TITLE_GLOW_MIN_DP +
                    (TITLE_GLOW_MAX_DP - TITLE_GLOW_MIN_DP) * breath) * density
                val glow = colors.first()
                gameTitle.setShadowLayer(
                    radius,
                    0f,
                    0f,
                    Color.argb(
                        (110 + 90 * breath).toInt(),
                        Color.red(glow),
                        Color.green(glow),
                        Color.blue(glow)
                    )
                )
            }
            start()
        }
    }

    /** Moves the light band across the title text ([t] in 0..1; idle part of the cycle). */
    private fun updateTitleShimmer(t: Float, colors: IntArray) {
        val width = gameTitle.width.toFloat()
        if (width <= 0f) return
        val band = width * 0.45f
        val key = width.toLong() * 31 + ConsolePalette.version
        if (key != shimmerKey || shimmerShader == null) {
            val accent = colors[0]
            val second = colors.getOrElse(1) { accent }
            shimmerShader = LinearGradient(
                0f, 0f, band, 0f,
                intArrayOf(Color.WHITE, accent, second, Color.WHITE),
                floatArrayOf(0f, 0.4f, 0.6f, 1f),
                Shader.TileMode.CLAMP
            )
            shimmerKey = key
        }
        // The band crosses during the first part of the cycle, then the title rests in white.
        val sweep = (t / TITLE_SWEEP_FRACTION).coerceAtMost(1f)
        shimmerMatrix.setTranslate(-band + sweep * (width + band), 0f)
        shimmerShader?.setLocalMatrix(shimmerMatrix)
        gameTitle.paint.shader = shimmerShader
        gameTitle.invalidate()
    }

    // ---------------------------------------------------------------------------------------------
    // Dynamic background

    private fun bindBackground(game: Game) {
        colorJob?.cancel()
        val cached = ConsoleGameColors.cached(game)
        if (cached != null) {
            ConsolePalette.rememberSelection(cached.base, cached.accents)
            liquidBackground.setPalette(cached.base, cached.accents)
            return
        }
        colorJob = fragment.viewLifecycleOwner.lifecycleScope.launch {
            val palette = ConsoleGameColors.load(fragment.viewLifecycleOwner, game)
                ?: return@launch
            if (!released && active && selectedGame == game) {
                ConsolePalette.rememberSelection(palette.base, palette.accents)
                liquidBackground.setPalette(palette.base, palette.accents)
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Console panel over Home (profile manager)

    private fun openOverlay(destination: SettingsSubscreen) {
        if (released) return
        menu?.dismiss()
        subscreenHost.open(destination, null)
        overlayTitle.setText(ConsoleSubscreenHost.titleOf(destination))
        overlay.alpha = 0f
        overlay.visibility = View.VISIBLE
        overlay.animate().alpha(1f).setDuration(OVERLAY_FADE_MS).start()
        setHomeContentVisible(false)
        overlayBackCallback.isEnabled = true
    }

    /** Home's own content (top bar, title, carousel) fades away under the console panel. */
    private fun setHomeContentVisible(visible: Boolean) {
        for (id in intArrayOf(R.id.header, R.id.console_details, R.id.swipe_refresh)) {
            val view = root.findViewById<View>(id) ?: continue
            view.animate().cancel()
            if (visible) {
                view.visibility = View.VISIBLE
                view.animate().alpha(1f).setDuration(OVERLAY_FADE_MS).start()
            } else {
                view.animate().alpha(0f).setDuration(OVERLAY_FADE_MS)
                    .withEndAction { view.visibility = View.INVISIBLE }
                    .start()
            }
        }
    }

    private fun onOverlayClosed() {
        overlayBackCallback.isEnabled = false
        overlay.animate().cancel()
        overlay.visibility = View.GONE
        if (released) return
        setHomeContentVisible(true)
        // The user may have switched profile or changed the avatar.
        loadAvatar()
        carousel.focusCenteredCard()
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

    /** Long click on a console tile shakes it and launches the game. */
    private fun launchSelected() {
        centeredItemView()?.performLongClick()
    }

    private fun handleCarouselKey(event: KeyEvent): Boolean {
        if (!active) return false
        return when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_X -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    launchSelected()
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
