package myau.events;

import myau.event.events.callables.EventCancellable;

/**
 * Stuck 模块专用 MoveEntityWithHeading 事件。
 * 在 EntityLivingBase.moveEntityWithHeading HEAD 派发，取消时跳过
 * 带朝向的移动逻辑，配合 StuckMoveEntityEvent 形成完整冻结。
 */
public class StuckMoveEntityWithHeadingEvent extends EventCancellable {
}
