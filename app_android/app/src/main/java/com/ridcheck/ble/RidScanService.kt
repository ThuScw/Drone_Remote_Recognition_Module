package com.ridcheck.ble

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import com.ridcheck.MainActivity
import com.ridcheck.R
import com.ridcheck.core.AppState
import com.ridcheck.core.Decoder

/**
 * 前台扫描服务：App 退到后台（例如 QGC 在前台控制无人机）时仍持续扫描并记录。
 * 拥有 BleScanner + 1Hz 采样 ticker；UI 活动通过 AppState 共享注册表只读展示。
 * 前台服务类型 connectedDevice（BLE），Android 15 起也无时长限制，可常驻。
 * START_STICKY：进程被系统回收后自动恢复并继续扫描。
 */
class RidScanService : Service() {

    companion object {
        const val CHANNEL_ID = "rid_scan"
        const val NOTIF_ID = 1001
        const val ACTION_START = "com.ridcheck.action.START_SCAN"
        const val ACTION_STOP = "com.ridcheck.action.STOP_SCAN"

        fun start(context: Context) {
            val i = Intent(context, RidScanService::class.java).setAction(ACTION_START)
            context.startForegroundService(i)
        }

        fun stop(context: Context) {
            val i = Intent(context, RidScanService::class.java).setAction(ACTION_STOP)
            context.startService(i)
        }
    }

    private var scanner: BleScanner? = null
    private var lastNotifCount = -1

    private val ticker = Handler(Looper.getMainLooper())
    private val tickRunnable = object : Runnable {
        override fun run() {
            AppState.registry.sampleAll(System.currentTimeMillis())
            updateNotification()
            ticker.postDelayed(this, 1000)
        }
    }

    private val listener = object : BleScanner.Listener {
        override fun onDistinctPacket(address: String, rssi: Int, raw: ByteArray) {
            val pkt = Decoder.decodeGbPacket(
                raw,
                address = address,
                rssi = rssi,
                receivedAtMs = System.nanoTime() / 1_000_000,
                source = "ble"
            )
            AppState.registry.onPacket(pkt, System.currentTimeMillis())
        }

        override fun onScanState(scanning: Boolean) {
            AppState.scanning = scanning
        }

        override fun onLog(text: String) {
            AppState.addLog(text)
        }
    }

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopScan()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            // ACTION_START，或系统回收后以 null intent 重启恢复
            else -> {
                startAsForeground()
                if (scanner == null) startScan()
                ticker.removeCallbacks(tickRunnable)
                ticker.post(tickRunnable)
                return START_STICKY
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        ticker.removeCallbacks(tickRunnable)
        stopScan()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun startScan() {
        val sc = BleScanner(listener)
        if (sc.start(this)) {
            scanner = sc
            AppState.scanning = true
        } else {
            AppState.addLog("后台扫描启动失败：无蓝牙或权限不足")
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    private fun stopScan() {
        scanner?.stop()
        scanner = null
        AppState.scanning = false
        AppState.registry.closeIssueTimelines(System.currentTimeMillis())
    }

    private fun startAsForeground() {
        val notif = buildNotification()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun updateNotification() {
        val count = AppState.registry.size
        if (count == lastNotifCount) return
        lastNotifCount = count
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        try {
            nm.notify(NOTIF_ID, buildNotification())
        } catch (e: SecurityException) {
            // POST_NOTIFICATIONS 未授权时通知不可见，但服务照常运行
        }
    }

    private fun buildNotification(): Notification {
        val count = AppState.registry.size
        val text = if (count == 0) {
            "等待 RID 广播设备（UUID 0x0D50）..."
        } else {
            "已记录 $count 台设备，后台持续收集中"
        }
        val contentIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle("RID 检测 · 后台扫描中")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(contentIntent)
            .build()
    }

    private fun createChannel() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "RID 后台扫描", NotificationManager.IMPORTANCE_LOW)
        )
    }
}
