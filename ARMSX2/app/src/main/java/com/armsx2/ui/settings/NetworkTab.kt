package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import java.net.NetworkInterface

/**
 * DEV9 networking/HDD settings brought over from OG ARMSX2's SettingsActivity.
 *
 * Android's useful backend is PCSX2's socket backend. PCAP options are kept
 * visible for parity/debugging, but normal users should leave the API on
 * Sockets and the adapter on Auto. DEV9 is initialized at VM boot, so these
 * settings are persisted immediately and take effect on the next game/BIOS
 * launch.
 */
@Composable
fun NetworkTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }
    val adapters = remember { enumerateAdapters() }
    val apiValues = listOf("Unset", "PCAP Bridged", "PCAP Switched", "Sockets")
    val apiLabels = listOf("Unset", "PCAP Br.", "PCAP Sw.", "Sockets")
    val apiIndex = apiValues.indexOf(s.dev9EthApi).let { if (it >= 0) it else apiValues.lastIndex }

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        Text(
            "PS2 network/HDD support. Restart the game or BIOS after changing DEV9.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        ToggleRow("Enable DEV9 Ethernet", s.dev9EthEnable) {
            val currentDevice = s.dev9EthDevice.ifEmpty { "Auto" }
            apply(
                s.copy(
                    dev9EthEnable = it,
                    dev9EthApi = s.dev9EthApi.ifEmpty { "Sockets" },
                    dev9EthDevice = currentDevice,
                )
            )
        }
        SettingsDivider()
        SegmentedRow(
            label = "Ethernet API",
            options = apiLabels,
            selectedIndex = apiIndex,
            onChange = { apply(s.copy(dev9EthApi = apiValues[it])) },
        )
        SettingsDivider()
        DeviceChooser(
            selected = s.dev9EthDevice.ifEmpty { "Auto" },
            adapters = adapters,
            onChange = { apply(s.copy(dev9EthDevice = it.ifEmpty { "Auto" })) },
        )
        SettingsDivider()
        ToggleRow("Enable DEV9 Virtual HDD", s.dev9HddEnable) {
            apply(s.copy(dev9HddEnable = it, dev9HddFile = s.dev9HddFile.ifEmpty { "DEV9hdd.raw" }))
        }
        SettingsDivider()
        HddFileRow(
            fileName = s.dev9HddFile.ifEmpty { "DEV9hdd.raw" },
            onReset = { apply(s.copy(dev9HddFile = "DEV9hdd.raw")) },
        )
    }
}

@Composable
private fun DeviceChooser(
    selected: String,
    adapters: List<String>,
    onChange: (String) -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .padding(horizontal = 6.dp, vertical = 4.dp),
    ) {
        Text("Ethernet Device", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(4.dp))
        adapters.forEach { adapter ->
            val active = adapter == selected
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(24.dp)
                    .clickable { onChange(adapter) }
                    .padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    adapter,
                    color = if (active) Colors.pasx2_blue else Color(0xFFCCCCCC),
                    fontSize = 12.sp,
                    fontWeight = if (active) FontWeight.Bold else FontWeight.Normal,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.weight(1f))
                if (active) {
                    Text("Selected", color = Colors.pasx2_blue, fontSize = 10.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}

@Composable
private fun HddFileRow(fileName: String, onReset: () -> Unit) {
    Box(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .clickable { onReset() }
            .padding(horizontal = 6.dp, vertical = 4.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.weight(1f)) {
                Text("HDD Image", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Text(fileName, color = Color(0xFFAAAAAA), fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Text("Reset", color = Colors.pasx2_blue, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        }
    }
}

private fun enumerateAdapters(): List<String> {
    val out = linkedSetOf("Auto")
    runCatching {
        val interfaces = NetworkInterface.getNetworkInterfaces() ?: return@runCatching
        interfaces.toList()
            .filter { iface ->
                runCatching {
                    iface.isUp && !iface.isLoopback && !iface.isVirtual
                }.getOrDefault(false)
            }
            .mapTo(out) { it.name }
    }
    return out.toList()
}
