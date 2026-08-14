package com.ridcheck

import com.ridcheck.core.GattConfig
import com.ridcheck.core.RidConfigData
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** GATT 配置校验测试。移植自 app/tests/test_gatt_config.py。 */
class GattConfigTest {

    @Test
    fun validateUasId() {
        assertTrue(GattConfig.validateUasId("1581FA6QC25B500C2H74")) // firmware placeholder
        assertTrue(GattConfig.validateUasId("0123456789ABCDEFGHJK"))
        assertFalse(GattConfig.validateUasId("1581FA6QC25B500C2H7O"))  // contains O
        assertFalse(GattConfig.validateUasId("1581FA6QC25B500C2H7I"))  // contains I
        assertFalse(GattConfig.validateUasId("1581FA6QC25B500C2H7a"))  // lowercase
        assertFalse(GattConfig.validateUasId("1581FA6QC25B500C2H7 "))  // space
        assertFalse(GattConfig.validateUasId("1581FA6QC25B500C2H7"))   // 19 chars
        assertFalse(GattConfig.validateUasId("1581FA6QC25B500C2H744")) // 21 chars
        assertFalse(GattConfig.validateUasId(""))                      // empty
    }

    @Test
    fun validateRealname() {
        assertTrue(GattConfig.validateRealname("07564244"))
        assertTrue(GattConfig.validateRealname("00000000"))
        assertFalse(GattConfig.validateRealname("0756424A"))  // letter
        assertFalse(GattConfig.validateRealname("0756424"))   // 7 chars
        assertFalse(GattConfig.validateRealname("075642440")) // 9 chars
        assertFalse(GattConfig.validateRealname("0756 244"))  // space
        assertFalse(GattConfig.validateRealname(""))          // empty
    }

    @Test
    fun validateOpCategory() {
        for (v in 0..3) assertTrue(GattConfig.validateOpCategory(v))
        assertFalse(GattConfig.validateOpCategory(-1))
        assertFalse(GattConfig.validateOpCategory(4))
    }

    @Test
    fun validateUaClass() {
        for (v in 0..4) assertTrue(GattConfig.validateUaClass(v))
        assertFalse(GattConfig.validateUaClass(-1))
        assertFalse(GattConfig.validateUaClass(5))
    }

    @Test
    fun ridConfigDataValidateCollectsErrors() {
        val cfg = RidConfigData(
            uasId = "1581FA6QC25B500C2H74", realname = "07564244",
            opCategory = 1, uaClass = 1, state = 1
        )
        assertTrue(cfg.validate().isEmpty())

        val bad = RidConfigData(uasId = "BAD", realname = "x", opCategory = 9, uaClass = 9)
        assertEquals(4, bad.validate().size)
    }

    @Test
    fun ridConfigDataStateProps() {
        val cfg = RidConfigData(state = 2)
        assertTrue(cfg.isAirborne)
        assertEquals("空中(写锁定)", cfg.stateName)

        val cfg2 = RidConfigData(state = 0)
        assertFalse(cfg2.isAirborne)
        assertEquals("未配置", cfg2.stateName)
    }

    @Test
    fun categoryLabelsMatchStandard() {
        assertEquals("未定义", GattConfig.OP_CATEGORY_NAMES[0])
        assertEquals("开放类", GattConfig.OP_CATEGORY_NAMES[1])
        assertEquals("特定类", GattConfig.OP_CATEGORY_NAMES[2])
        assertEquals("审定类", GattConfig.OP_CATEGORY_NAMES[3])
        assertEquals("轻型无人驾驶航空器", GattConfig.UA_CLASS_NAMES[1])
        assertEquals("大型无人驾驶航空器", GattConfig.UA_CLASS_NAMES[4])
    }
}
