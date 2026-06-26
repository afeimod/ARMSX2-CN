package com.armsx2.ui

import androidx.annotation.StringRes
import com.armsx2.R

/**
 * Centralised labels for the in-game quick menu (the overlay that opens when
 * the user presses the Android system BACK key while a game is running).
 *
 * The quick menu itself is the existing [InGameOverlay] (PlayingNow tab);
 * the BACK hook lives in [com.armsx2.Main.dispatchKeyEvent]. Strings live
 * here so they can be re-used by the per-button content descriptions and so
 * that future i18n work has a single place to update.
 */
object QuickMenuStrings {
    /** Shown in the overlay header when the menu was opened from the BACK key. */
    @StringRes val titleRes: Int = R.string.quick_menu_title

    /** Per-cell content descriptions for the PlayingNow quick-menu grid. */
    @StringRes val cells: IntArray = intArrayOf(
        R.string.quick_menu_resume,
        R.string.quick_menu_save_state,
        R.string.quick_menu_load_state,
        R.string.quick_menu_swap_disc,
        R.string.quick_menu_boot_disc,
        R.string.quick_menu_library,
        R.string.quick_menu_renderer,
        R.string.quick_menu_frame_limit,
        R.string.quick_menu_touch_layout,
        R.string.quick_menu_osd,
        R.string.quick_menu_reset,
        R.string.quick_menu_close_game,
    )
}