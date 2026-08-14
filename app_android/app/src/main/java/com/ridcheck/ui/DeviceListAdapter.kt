package com.ridcheck.ui

import android.content.Context
import android.view.View
import android.view.ViewGroup
import com.ridcheck.core.Decoder
import com.ridcheck.core.DeviceEntry

/**
 * 设备列表行 adapter。
 * 每行：健康级别● + 设备地址 / RSSI + 运行状态 + 最后更新时间。
 * devices 用 lambda 惰性读取，避免持有过期引用。
 */
class DeviceListAdapter(
    context: Context,
    devices: () -> List<DeviceEntry>
) : CardListAdapter<DeviceEntry>(context, devices) {

    override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val entry = getItem(position)
        val row = (convertView as? android.widget.LinearLayout) ?: buildRow()
        row.removeAllViews()

        val now = System.currentTimeMillis()
        val level = entry.assessor.report().level
        row.addView(topLine(level, entry.address))

        val opStatus = entry.lastPkt?.opStatus
        val opText = if (opStatus != null) Decoder.OP_STATUS[opStatus] ?: "无效($opStatus)" else "-"
        row.addView(bottomLine("${entry.rssi}dBm | $opText | ${agoText(now - entry.lastSeenMs)}"))
        return row
    }
}
