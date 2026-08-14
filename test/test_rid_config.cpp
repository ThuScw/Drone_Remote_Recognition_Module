// GATT 可配置字段校验纯函数 (rid_config.h)
#include "test_common.h"
#include "rid_config.h"

void test_rid_config() {
    printf("--- RidConfig validation ---\n");

    // ==== 001 唯一产品识别码: 20 字符 [0-9A-Z] 禁 O/I ====
    CHECK(ridConfigValidateUasId("1581FA6QC25B500C2H74", 20));   // 占位值本身合法
    CHECK(ridConfigValidateUasId("0123456789ABCDEFGHJK", 20));   // 全合法字符
    CHECK(!ridConfigValidateUasId("1581FA6QC25B500C2H7O", 20));  // 含 O
    CHECK(!ridConfigValidateUasId("1581FA6QC25B500C2H7I", 20));  // 含 I
    CHECK(!ridConfigValidateUasId("1581FA6QC25B500C2H7a", 20));  // 含小写
    CHECK(!ridConfigValidateUasId("1581FA6QC25B500C2H7 ", 20));  // 含空格
    CHECK(!ridConfigValidateUasId("1581FA6QC25B500C2H7", 19));   // 短 1
    CHECK(!ridConfigValidateUasId("1581FA6QC25B500C2H744", 21)); // 长 1
    CHECK(!ridConfigValidateUasId("", 0));                        // 空

    // ==== 002 实名登记标志: 8 位数字 ====
    CHECK(ridConfigValidateRealName("07564244", 8));
    CHECK(ridConfigValidateRealName("00000000", 8));
    CHECK(!ridConfigValidateRealName("0756424A", 8));   // 含字母
    CHECK(!ridConfigValidateRealName("0756424", 7));    // 短 1
    CHECK(!ridConfigValidateRealName("075642440", 9));  // 长 1
    CHECK(!ridConfigValidateRealName("0756 244", 8));   // 含空格
    CHECK(!ridConfigValidateRealName("", 0));

    // ==== 003 运行类别: 0~3 ====
    CHECK(ridConfigValidateOpCategory(0));
    CHECK(ridConfigValidateOpCategory(1));
    CHECK(ridConfigValidateOpCategory(2));
    CHECK(ridConfigValidateOpCategory(3));
    CHECK(!ridConfigValidateOpCategory(4));

    // ==== 004 无人机分类: 0~4 ====
    CHECK(ridConfigValidateUaClass(0));
    CHECK(ridConfigValidateUaClass(1));
    CHECK(ridConfigValidateUaClass(4));
    CHECK(!ridConfigValidateUaClass(5));
}
