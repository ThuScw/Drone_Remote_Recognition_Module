package com.ridcheck.ui

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.ridcheck.core.GB_FIELD_TABLE

/** 静态说明页：GB 46750-2025 远程识别简介、表3 字段、判定标准与使用方法。 */
object ExplainPage {

    fun build(context: Context): ScrollView {
        val scroll = ScrollView(context)
        val col = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(context, 12), dp(context, 8), dp(context, 12), dp(context, 24))
        }
        scroll.addView(
            col,
            ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        )

        col.addView(title(context, "RID 检测（GB 46750-2025）使用说明"))
        col.addView(body(
            context,
            "本 APP 面向无人机企业研发与合规自查：扫描或粘贴远程识别（RID）广播数据，" +
                "逐字段解码并对照 GB 46750-2025 给出合规判定。"
        ))

        col.addView(section(context, "什么是远程识别（RID）？"))
        col.addView(body(
            context,
            "通俗讲：无人机在空中向周边广播自己的身份、位置和状态，类似飞机的应答机。"
        ))
        col.addView(body(
            context,
            "技术讲：本 APP 监听蓝牙 BLE 5.0 扩展广播（Service UUID 0x0D50，dataType 0xFF，" +
                "版本字节 0x20 即 V1.0），从广播帧中提取并解析 GB 46750-2025 规定的数据包。"
        ))

        col.addView(section(context, "表3 广播信息字段（21 项，M 必选 / O 可选）"))
        col.addView(fields(context))

        col.addView(section(context, "合规判定标准"))
        col.addView(body(
            context,
            "5.1.2 全程连续广播：无人机从起飞到降落全程持续广播，中途中断判为故障（STALE）。"
        ))
        col.addView(body(
            context,
            "5.1.3 广播间隔 ≤1 秒：低于 1 包/秒判为警告（RATE），长期低于 0.5 包/秒判为故障。"
        ))
        col.addView(body(
            context,
            "表3 字段要求：必填字段缺失、取值越界或格式不符，按影响程度判为警告或故障，并附具体条款。"
        ))

        col.addView(section(context, "判定等级"))
        col.addView(body(context, "● 正常（PASS）：广播符合 GB 46750-2025 要求。"))
        col.addView(body(context, "● 警告（WARN）：存在可改善项，不影响基本广播（如旧版本、可选项缺失）。"))
        col.addView(body(context, "● 故障（FAIL）：存在影响合规或安全的严重问题（如关键字段缺失、广播中断）。"))

        col.addView(section(context, "使用方法"))
        col.addView(body(context, "1. 主界面点“开始扫描”，信号源列表实时出现广播设备；点设备进详情。"))
        col.addView(body(context, "2. 详情页：查看逐字段解码、问题清单、RSSI/速率曲线；复制或分享原始 HEX。"))
        col.addView(body(context, "3. 详情页可生成该设备的合规报告，或导出历史采样 CSV（可用 Excel/WPS 打开）。"))
        col.addView(body(context, "4. “粘贴解码”：粘贴 nRF Connect 等工具抓到的 HEX，进行单包静态解码，不参与扫描统计。"))

        col.addView(section(context, "免责声明"))
        col.addView(body(
            context,
            "本 APP 为研发检测工具，判定结果仅供合规自查参考，不构成官方检测结论；" +
                "具体合规判定以 GB 46750-2025 标准原文及具备资质的检测机构为准。"
        ))

        return scroll
    }

    private fun title(context: Context, text: String): TextView = TextView(context).apply {
        this.text = text
        textSize = 18f
        setTypeface(null, Typeface.BOLD)
        setTextColor(Theme.PRIMARY)
        setPadding(0, 0, 0, dp(context, 6))
    }

    private fun section(context: Context, text: String): TextView = TextView(context).apply {
        this.text = text
        textSize = 14f
        setTypeface(null, Typeface.BOLD)
        setTextColor(Theme.PRIMARY)
        setPadding(0, dp(context, 12), 0, dp(context, 2))
    }

    private fun body(context: Context, text: String): TextView = TextView(context).apply {
        this.text = text
        textSize = 13f
        setTextColor(Color.rgb(70, 70, 70))
        setLineSpacing(dp(context, 4).toFloat(), 1.0f)
        setPadding(0, dp(context, 2), 0, dp(context, 2))
    }

    private fun fields(context: Context): TextView = TextView(context).apply {
        text = GB_FIELD_TABLE.joinToString("\n") { f ->
            "${f.num} ${f.name.padEnd(10, '　')} ${if (f.optional) "O" else "M"}"
        }
        textSize = 12f
        setTypeface(Typeface.MONOSPACE, Typeface.NORMAL)
        setTextColor(Color.rgb(90, 90, 90))
        setLineSpacing(dp(context, 3).toFloat(), 1.0f)
        setPadding(dp(context, 4), dp(context, 4), dp(context, 4), dp(context, 4))
    }

    private fun dp(context: Context, v: Int): Int =
        (v * context.resources.displayMetrics.density).toInt()
}
