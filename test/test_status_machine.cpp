// 状态机: 运行状态消抖决策纯函数 (status_machine.h)
#include "test_common.h"
#include "status_machine.h"

void test_status_machine() {
    printf("--- Status Machine ---\n");

    // ==== 1. 地面→空中: 300ms 边界 ====
    {
        DebounceState st;
        st.confirmed = STATUS_GROUND;
        StatusStepResult r;
        r = statusStep(st, STATUS_AIRBORNE, 0);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_AIRBORNE, DEBOUNCE_MS_GND_AIR - 1);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_AIRBORNE, DEBOUNCE_MS_GND_AIR);
        CHECK_EQ(r, StatusStepResult::DEBOUNCED);
        CHECK_EQ(st.confirmed, STATUS_AIRBORNE);
        CHECK_EQ(st.target, 0xFF);   // 消抖完成, 目标已清
        CHECK_EQ(st.startMs, 0ULL);
    }

    // ==== 2. 空中→地面: 500ms 更严格 (不对称阈值) ====
    {
        DebounceState st;
        st.confirmed = STATUS_AIRBORNE;
        StatusStepResult r;
        r = statusStep(st, STATUS_GROUND, 100);
        CHECK_EQ(r, StatusStepResult::PENDING);
        // 300ms 时仍不应切换 — 若误用 GND_AIR 阈值会提前停播 (500ms 才是空中→地面)
        r = statusStep(st, STATUS_GROUND, 100 + DEBOUNCE_MS_GND_AIR);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_GROUND, 100 + DEBOUNCE_MS_AIR_GND - 1);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_GROUND, 100 + DEBOUNCE_MS_AIR_GND);
        CHECK_EQ(r, StatusStepResult::DEBOUNCED);
        CHECK_EQ(st.confirmed, STATUS_GROUND);
    }

    // ==== 3. 紧急/失效绕过消抖立即切换 ====
    {
        DebounceState st;
        st.confirmed = STATUS_GROUND;
        StatusStepResult r;
        r = statusStep(st, STATUS_EMERGENCY, 0);
        CHECK_EQ(r, StatusStepResult::EMERGENCY);
        CHECK_EQ(st.confirmed, STATUS_EMERGENCY);
        CHECK_EQ(st.target, 0xFF);   // 紧急不进入消抖
        CHECK_EQ(st.startMs, 0ULL);

        DebounceState st2;
        st2.confirmed = STATUS_GROUND;
        r = statusStep(st2, STATUS_FAIL_SAFE, 0);
        CHECK_EQ(r, StatusStepResult::EMERGENCY);
        CHECK_EQ(st2.confirmed, STATUS_FAIL_SAFE);

        DebounceState st3;
        st3.confirmed = STATUS_GROUND;
        r = statusStep(st3, STATUS_FAIL_EMERG, 0);
        CHECK_EQ(r, StatusStepResult::EMERGENCY);
        CHECK_EQ(st3.confirmed, STATUS_FAIL_EMERG);
    }

    // ==== 4. 抖动: 中断的 300ms 窗口被重置 ====
    {
        DebounceState st;
        st.confirmed = STATUS_GROUND;
        StatusStepResult r;
        r = statusStep(st, STATUS_AIRBORNE, 0);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_GROUND, 100);   // 回弹到确认状态
        CHECK_EQ(r, StatusStepResult::UNCHANGED);
        CHECK_EQ(st.target, 0xFF);                // 消抖已重置
        r = statusStep(st, STATUS_AIRBORNE, 200); // 重新计时
        CHECK_EQ(r, StatusStepResult::PENDING);
        CHECK_EQ(st.startMs, 200ULL);
        r = statusStep(st, STATUS_AIRBORNE, 200 + DEBOUNCE_MS_GND_AIR - 1);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_AIRBORNE, 200 + DEBOUNCE_MS_GND_AIR);
        CHECK_EQ(r, StatusStepResult::DEBOUNCED);
    }

    // ==== 5. 状态一致重置消抖 (候选 == 确认) ====
    {
        DebounceState st;
        st.confirmed = STATUS_GROUND;
        statusStep(st, STATUS_AIRBORNE, 0);  // 进入消抖
        CHECK_EQ(st.target, STATUS_AIRBORNE);
        StatusStepResult r;
        r = statusStep(st, STATUS_GROUND, 200);  // 候选回落为确认状态
        CHECK_EQ(r, StatusStepResult::UNCHANGED);
        CHECK_EQ(st.target, 0xFF);
        CHECK_EQ(st.startMs, 0ULL);
        CHECK_EQ(st.confirmed, STATUS_GROUND);
    }

    // ==== 6. UNREPORTED(0) 从空中来: 非 GROUND, 走 300ms 阈值 ====
    {
        DebounceState st;
        st.confirmed = STATUS_AIRBORNE;
        StatusStepResult r;
        r = statusStep(st, STATUS_UNREPORTED, 0);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_UNREPORTED, DEBOUNCE_MS_GND_AIR - 1);
        CHECK_EQ(r, StatusStepResult::PENDING);
        r = statusStep(st, STATUS_UNREPORTED, DEBOUNCE_MS_GND_AIR);
        CHECK_EQ(r, StatusStepResult::DEBOUNCED);
        CHECK_EQ(st.confirmed, STATUS_UNREPORTED);
    }

    // ==== 7. 初始状态 0xFF ====
    {
        DebounceState st;  // confirmed=0xFF, target=0xFF, startMs=0
        StatusStepResult r;
        // 0xFF 表示"未知", 首个真实候选直接进入消抖
        r = statusStep(st, STATUS_AIRBORNE, 0);
        CHECK_EQ(r, StatusStepResult::PENDING);
        CHECK_EQ(st.target, STATUS_AIRBORNE);
        CHECK_EQ(st.startMs, 0ULL);
    }
}
