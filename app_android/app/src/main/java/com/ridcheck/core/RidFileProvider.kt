package com.ridcheck.core

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File

/**
 * 零 AndroidX 的文件分享 ContentProvider：
 * 把 content://com.ridcheck.files/<name> 映射到 filesDir/csv/<name>，供 CSV 导出走系统分享。
 * 只读打开，路径校验防穿越；query/insert/update/delete 一律空实现且不抛异常。
 */
class RidFileProvider : ContentProvider() {

    override fun onCreate(): Boolean = true

    override fun getType(uri: Uri): String? =
        if (uri.lastPathSegment != null) "text/csv" else null

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        val name = uri.lastPathSegment ?: return null
        if (name.isEmpty() || name.contains("..") || name.contains('/') || name.contains('\\')) {
            return null
        }
        val ctx = context ?: return null
        val f = File(File(ctx.filesDir, "csv"), name)
        if (!f.exists() || !f.isFile) return null
        return try {
            ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
        } catch (e: Exception) {
            null
        }
    }

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?
    ): Cursor? = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<out String>?
    ): Int = 0

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0
}
