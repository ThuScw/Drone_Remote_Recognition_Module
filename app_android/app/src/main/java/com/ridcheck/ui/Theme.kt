package com.ridcheck.ui

import android.graphics.Color

/** 全局紫色系色板：品牌主色为深紫，判定 PASS 亦用紫色系；警告/故障保留琥珀/红便于识别。 */
object Theme {
    // 品牌主色
    val PRIMARY = Color.rgb(94, 53, 177)       // 深紫 #5E35B1
    val PRIMARY_SOFT = Color.rgb(237, 231, 246) // 浅紫底 #EDE7F6
    val CARD_BG = Color.rgb(244, 241, 252)     // 卡片浅紫 #F4F1FC

    // 判定状态色
    val PASS = Color.rgb(94, 53, 177)
    val PASS_BG = Color.rgb(237, 231, 246)
    val WARN = Color.rgb(178, 106, 0)
    val WARN_BG = Color.rgb(255, 248, 225)
    val FAIL = Color.rgb(183, 28, 28)
    val FAIL_BG = Color.rgb(255, 235, 238)

    // 图表第二轴（速率）
    val RATE = Color.rgb(230, 126, 34)

    // 文本色阶
    val TEXT_DARK = Color.rgb(30, 30, 30)        // 卡片标题/正文
    val TEXT_SECONDARY = Color.rgb(80, 80, 80)   // 次要文字/日志
    val TEXT_MUTED = Color.rgb(102, 102, 102)    // 副行/占位
    val TEXT_INACTIVE = Color.rgb(120, 120, 120) // 底部栏未选中 tab
}
