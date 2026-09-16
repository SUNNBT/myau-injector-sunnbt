package myau.events;

import myau.event.events.callables.EventCancellable;

/**
 * Stuck 模块专用 MoveEntity 事件。
 * 在 Entity.moveEntity HEAD 派发，取消时直接跳过本帧实体位移，
 * 从而在不操作 motion 的情况下“卡住”玩家位置。
 */
public class StuckMoveEntityEvent extends EventCancellable {
}
