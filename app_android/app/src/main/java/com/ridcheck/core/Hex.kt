package com.ridcheck.core

/** 字节 → 空格分隔的大写十六进制（如 "FE 20 05"）。 */
fun ByteArray.toHexSpaced(): String =
    joinToString(" ") { String.format("%02X", it.toInt() and 0xFF) }

/** 字节 → 连续大写十六进制（如 "FE2005"）。 */
fun ByteArray.toHexJoined(): String =
    joinToString("") { String.format("%02X", it.toInt() and 0xFF) }
