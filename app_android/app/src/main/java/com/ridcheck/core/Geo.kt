package com.ridcheck.core

import kotlin.math.cos
import kotlin.math.sqrt

/**
 * 等距圆柱投影（米）：把经纬度差值换算为相对某基准点的 N/E 米。
 * 会话轨迹最远距离（SessionStats）与相对轨迹图（ChartPng）共用同一套系数。
 * 纯 JVM，无 android 依赖。
 */
object Geo {
    const val METERS_PER_DEG_LAT = 110574.0
    const val METERS_PER_DEG_LON_AT_EQ = 111320.0

    /** 东向位移（米）：x = (lon - lon0) * cos(lat0) * 111320。 */
    fun xMeters(lonDeg: Double, lon0Deg: Double, lat0Deg: Double): Double =
        (lonDeg - lon0Deg) * cos(Math.toRadians(lat0Deg)) * METERS_PER_DEG_LON_AT_EQ

    /** 北向位移（米）：y = (lat - lat0) * 110574。 */
    fun yMeters(latDeg: Double, lat0Deg: Double): Double =
        (latDeg - lat0Deg) * METERS_PER_DEG_LAT

    /** 两点间近似距离（米，等距圆柱）。 */
    fun distMeters(lat0Deg: Double, lon0Deg: Double, lat1Deg: Double, lon1Deg: Double): Double {
        val x = xMeters(lon1Deg, lon0Deg, lat0Deg)
        val y = yMeters(lat1Deg, lat0Deg)
        return sqrt(x * x + y * y)
    }
}
