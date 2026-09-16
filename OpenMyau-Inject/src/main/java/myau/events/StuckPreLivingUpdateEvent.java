package myau.events;

import myau.event.events.callables.EventCancellable;

/**
 * Stuck 模块专用 PreLivingUpdate 事件。
 * 在 EntityPlayerSP.onLivingUpdate HEAD 派发，用于 Stuck 模块
 * 在任何 living 逻辑之前刷新 pulse 计时与 hurt 透传状态。
 */
public class StuckPreLivingUpdateEvent extends EventCancellable {
}
