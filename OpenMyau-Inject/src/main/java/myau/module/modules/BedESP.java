package myau.module.modules;

import java.awt.Color;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArraySet;

import myau.Myau;
import myau.event.EventTarget;
import myau.event.types.EventType;
import myau.events.Render3DEvent;
import myau.events.TickEvent;
import myau.module.Module;
import myau.property.properties.BooleanProperty;
import myau.property.properties.ColorProperty;
import myau.property.properties.FloatProperty;
import myau.property.properties.ModeProperty;
import myau.util.RenderUtil;
import net.minecraft.block.BlockBed;
import net.minecraft.block.BlockBed.EnumPartType;
import net.minecraft.block.BlockObsidian;
import net.minecraft.block.state.IBlockState;
import net.minecraft.client.Minecraft;
import net.minecraft.util.BlockPos;
import net.minecraft.util.EnumFacing;
import net.minecraft.util.MathHelper;

public class BedESP extends Module {
    private static final Minecraft mc = Minecraft.getMinecraft();
    // 扫描器(WorldCallbacks/MixinBlockRendererDispatcher)只往这里放床头位置
    public final CopyOnWriteArraySet<BlockPos> beds = new CopyOnWriteArraySet<>();
    // 能见距离(格)
    public final FloatProperty range = new FloatProperty("range", 20.0F, 4.0F, 128.0F, 1.0F);
    public final ModeProperty color = new ModeProperty("color", 0, new String[]{"CUSTOM", "HUD"});
    public final ColorProperty customColor;
    // 黑曜石紫色描边开关
    public final BooleanProperty obsidian = new BooleanProperty("obsidian", true);

    // onTick 扫描间隔(tick)与最大扫描半径(避免大 range 时卡顿)
    private static final int SCAN_INTERVAL = 4;
    private static final int MAX_SCAN_RADIUS = 24;
    private int scanCounter = 0;

    // 黑曜石描边颜色：紫色
    private static final int OBS_R = 170;
    private static final int OBS_G = 0;
    private static final int OBS_B = 170;
    private static final float LINE_WIDTH = 1.5F;
    // 床周围需要检查的方向(与原实现一致：上方+四侧)
    private static final EnumFacing[] NEIGHBOR_FACES = {
            EnumFacing.UP, EnumFacing.NORTH, EnumFacing.EAST, EnumFacing.SOUTH, EnumFacing.WEST
    };

    private Color getColor() {
        switch (this.color.getValue()) {
            case 0:
                return new Color(this.customColor.getValue());
            case 1:
                return ((HUD) Myau.moduleManager.modules.get(HUD.class)).getColor(System.currentTimeMillis());
            default:
                return new Color(-1);
        }
    }

    public BedESP() {
        super("Bed ESP", false);
        this.customColor = new ColorProperty("custom-color", (int) 8085714755840333141L, () -> this.color.getValue() == 0);
    }

    @EventTarget
    public void onTick(TickEvent event) {
        if (event.getType() != EventType.PRE || !this.isEnabled() || mc.theWorld == null || mc.thePlayer == null) {
            return;
        }
        if (++this.scanCounter < SCAN_INTERVAL) {
            return;
        }
        this.scanCounter = 0;
        // 补充扫描：被方块包围的床不会触发 renderBlock，直接遍历玩家周围方块
        int radius = Math.min(MAX_SCAN_RADIUS, (int) Math.ceil(this.range.getValue()));
        int px = MathHelper.floor_double(mc.thePlayer.posX);
        int py = MathHelper.floor_double(mc.thePlayer.posY);
        int pz = MathHelper.floor_double(mc.thePlayer.posZ);
        for (int x = px - radius; x <= px + radius; x++) {
            for (int y = py - radius; y <= py + radius; y++) {
                for (int z = pz - radius; z <= pz + radius; z++) {
                    BlockPos pos = new BlockPos(x, y, z);
                    IBlockState state = mc.theWorld.getBlockState(pos);
                    if (state.getBlock() instanceof BlockBed
                            && state.getValue(BlockBed.PART) == EnumPartType.HEAD) {
                        this.beds.add(pos);
                    }
                }
            }
        }
    }

    @EventTarget
    public void onRender3D(Render3DEvent event) {
        if (!this.isEnabled() || mc.theWorld == null || mc.thePlayer == null) {
            return;
        }

        final double rangeSq = this.range.getValue().doubleValue() * this.range.getValue().doubleValue();
        final double px = mc.thePlayer.posX;
        final double py = mc.thePlayer.posY;
        final double pz = mc.thePlayer.posZ;

        RenderUtil.enableRenderState();
        try {
            for (BlockPos head : this.beds) {
                final IBlockState state = mc.theWorld.getBlockState(head);
                if (!(state.getBlock() instanceof BlockBed)
                        || state.getValue(BlockBed.PART) != EnumPartType.HEAD) {
                    this.beds.remove(head);
                    continue;
                }

                final BlockPos foot = head.offset(state.getValue(BlockBed.FACING).getOpposite());
                final IBlockState footState = mc.theWorld.getBlockState(foot);
                if (!(footState.getBlock() instanceof BlockBed)
                        || footState.getValue(BlockBed.PART) != EnumPartType.FOOT) {
                    continue;
                }

                // 能见距离：以床中心到玩家的距离判定
                final double cx = (head.getX() + foot.getX()) * 0.5 + 0.5;
                final double cy = head.getY() + 0.5;
                final double cz = (head.getZ() + foot.getZ()) * 0.5 + 0.5;
                final double dx = cx - px;
                final double dy = cy - py;
                final double dz = cz - pz;
                if (dx * dx + dy * dy + dz * dz > rangeSq) {
                    continue;
                }

                // 一张床 = 两个独立的满方块线框(头、脚各一个 1x1x1)
                final Color bedColor = this.getColor();
                RenderUtil.drawBlockBoundingBox(head, 1.0,
                        bedColor.getRed(), bedColor.getGreen(), bedColor.getBlue(), 255, LINE_WIDTH);
                RenderUtil.drawBlockBoundingBox(foot, 1.0,
                        bedColor.getRed(), bedColor.getGreen(), bedColor.getBlue(), 255, LINE_WIDTH);

                // 紧贴床的黑曜石：每块一个紫色满方块线框
                if (this.obsidian.getValue()) {
                    final Set<BlockPos> checked = new HashSet<>();
                    for (BlockPos part : Arrays.asList(head, foot)) {
                        for (EnumFacing facing : NEIGHBOR_FACES) {
                            final BlockPos neighbor = part.offset(facing);
                            if (!checked.add(neighbor)) {
                                continue;
                            }
                            if (mc.theWorld.getBlockState(neighbor).getBlock() instanceof BlockObsidian) {
                                RenderUtil.drawBlockBoundingBox(neighbor, 1.0,
                                        OBS_R, OBS_G, OBS_B, 255, LINE_WIDTH);
                            }
                        }
                    }
                }
            }
        } finally {
            RenderUtil.disableRenderState();
        }
    }

    @Override
    public void onEnabled() {
        if (mc.renderGlobal != null) {
            mc.renderGlobal.loadRenderers();
        }
    }
}
