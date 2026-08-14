package com.ridcheck.ui

import android.content.Context
import android.view.View
import android.view.ViewGroup
import com.ridcheck.core.ConfigDevice

/**
 * 可 GATT 配置模块列表行 adapter。
 * 每行：● + 设备地址 / RSSI + 广播名 + 最后更新时间。
 * devices 用 lambda 惰性读取，避免持有过期引用。
 */
class ConfigDeviceAdapter(
    context: Context,
    devices: () -> List<ConfigDevice>
) : CardListAdapter<ConfigDevice>(context, devices) {

    override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val entry = getItem(position)
        val row = (convertView as? android.widget.LinearLayout) ?: buildRow(Theme.PRIMARY_SOFT)
        row.removeAllViews()

        row.addView(topLine(null, entry.address))

        val name = entry.name ?: "(无)"
        row.addView(
            bottomLine("${entry.rssi}dBm | $name | ${agoText(System.currentTimeMillis() - entry.lastSeenMs)}")
        )
        return row
    }
}
