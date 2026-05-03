package com.armsx2.ui.toolbar

import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.vector.ImageVector
import com.armsx2.ui.WindowImpl
import compose.icons.LineAwesomeIcons
import compose.icons.lineawesomeicons.FolderOpen

class ToggleLibraryButton : ToolbarButton() {
    override var icon = mutableStateOf<ImageVector?>(LineAwesomeIcons.FolderOpen)
    override fun isVisible(): Boolean {
        return true
    }

    override fun action() {
        // Toggle the GamesList library overlay. When stopped, GamesList is
        // already painted by Main.kt's idle-state branch so this flag is
        // only consulted while a game is running — flipping it on overlays
        // the library card grid, flipping it off restores the surface view.
        // GamesList.GameCard taps clear the flag automatically after they
        // call launchGame (so the user sees the new game come up cleanly).
        WindowImpl.showLibrary.value = !WindowImpl.showLibrary.value
    }
}