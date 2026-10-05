/*
 * How many depth planes this frame can be split into.
 *
 * A layer that blends with whatever lies beneath it has to keep that beneath
 * in the same surface, so everything from its priority down stays together.
 * That is GBA alpha blending, which needs a target-1 layer, a target-2 layer
 * and a nonzero second coefficient (the overworld leaves the effect enabled
 * with no target-1 layer at all, which blends nothing), and a sprite in the
 * semi-transparent OBJ mode, which blends whatever its own priority allows.
 *
 * The result is planes 0..count-2 holding one priority each and the last
 * holding the rest, so the planes never change what a frame looks like.
 */
/* The depth slots this frame actually draws something in. */
static unsigned UsedSlots(void)
{
    unsigned display = Reg(0), mode = display & 7, used = 0;

    for (unsigned bg = 0; bg < 4; ++bg)
    {
        if (!(display & (0x100u << bg))) continue;
        if ((mode == 1 && bg == 3) || (mode == 2 && bg < 2)) continue;
        used |= SLOT_BG(Reg(8 + bg * 2) & 3);
    }
    if (display & 0x1000)
        for (unsigned i = 0; i < 128; ++i)
        {
            unsigned attr0 = sMemory.oam[i * 4];

            if (!(attr0 & 0x100) && (attr0 & 0x200)) continue;
            if (((attr0 >> 10) & 3) >= 2) continue;
            used |= SLOT_OBJ((sMemory.oam[i * 4 + 2] >> 10) & 3);
        }
    return used;
}

/*
 * The slots of each plane, front to back: every slot in use its own plane,
 * from the nearest, while planes last; the last plane takes all the rest, and
 * everything from priority `merge` back (layers scrolled together) is one.
 * So the depths go where the picture has something - the intro's bike scene
 * is its sprites, its near layer and its far ones, not three priorities of
 * which the first is empty.
 */
static unsigned sBandSlots[CTR_BANDS];

static unsigned BandsFromSlots(unsigned used, unsigned merge)
{
    unsigned count = 0, cut = merge < 4 ? SLOT_OBJ(merge) : 256u, done = 0;
    /*
     * What is in front of layers scrolled together keeps a plane of its own
     * even when planes are short: the field's text window shares priority 0
     * with sprites, and with two planes the sprites took the near one and
     * left the window flat on the map.
     */
    unsigned front = sBandCount - (cut < 256 && (used & ~(cut - 1)) ? 1 : 0);

    if (!sBandCount) return 0;
    for (unsigned bit = 1; bit < cut; bit <<= 1)
    {
        if (!(used & bit)) continue;
        if (count + 1 < front) sBandSlots[count++] = bit;
        else
            /* The last plane before the cut: this slot and all up to it. */
            sBandSlots[count++] = (cut - 1) & ~(bit - 1);
        done = (bit << 1) - 1;
        if (count == front) { done = cut - 1; break; }
    }
    /* The last plane: everything behind what has a plane already. */
    if (count < sBandCount && (used & ~done)) sBandSlots[count++] = SLOTS_ALL & ~done;
    else if (count) sBandSlots[count - 1] |= SLOTS_ALL & ~done;
    if (count == 0) sBandSlots[count++] = SLOTS_ALL;
    /* The first plane also takes the empty slots in front of it. */
    sBandSlots[0] |= (sBandSlots[0] & -sBandSlots[0]) - 1;
    return count;
}

static unsigned DepthPlanes(void)
{
    unsigned display = Reg(0);
    unsigned merge = 4;

    /*
     * Blending no longer merges planes: a plane with something that blends
     * gets what lies behind it underneath, colour only (RenderBands).
     */
    /*
     * Backgrounds scrolled together are one image cut into layers, like the
     * three metatile layers of the field, and giving them different depths
     * pulls their tiles a pixel apart. Only a shared nonzero scroll means
     * that: on a still screen every layer sits at zero and they are separate
     * pictures stacked on each other, which is exactly where depth belongs.
     */
    for (unsigned bg = 0; bg + 1 < 4 && merge; ++bg)
        for (unsigned other = bg + 1; other < 4 && merge; ++other)
        {
            unsigned scroll = Reg(0x10 + bg * 4) | (Reg(0x12 + bg * 4) << 16);
            unsigned priority, otherPriority;

            if (!(display & (0x100u << bg)) || !(display & (0x100u << other))) continue;
            if (!scroll || scroll != (Reg(0x10 + other * 4) | (Reg(0x12 + other * 4) << 16)))
                continue;
            priority = Reg(8 + bg * 2) & 3;
            otherPriority = Reg(8 + other * 2) & 3;
            if (otherPriority < priority) priority = otherPriority;
            if (priority < merge) merge = priority;
        }
    /*
     * The slots stay in use for the rest of the scene: a sprite that comes
     * and goes - the logo's letters, a sparkle - would otherwise move every
     * layer behind it from one plane to the next and back, the depth of the
     * whole picture flickering with it. A scene is its display control and
     * background priorities; when they change, so may the depths.
     */
    {
        static unsigned sticky, stickyKey;
        unsigned key = (display & 0x1f07) | (Reg(8) & 3) << 16 | (Reg(10) & 3) << 18
                     | (Reg(12) & 3) << 20 | (Reg(14) & 3) << 22 | (unsigned)sStage << 24;

        if (key != stickyKey) sticky = 0;
        stickyKey = key;
        sticky |= UsedSlots();
        return BandsFromSlots(sticky, merge);
    }
}

/*
 * The depth slots holding something that blends with what lies beneath it:
 * a first target of BLDCNT's alpha blend, or a semi-transparent sprite.
 */
static unsigned BlendingPriorities(void)
{
    unsigned display = Reg(0), control = Reg(0x50), found = 0;
    unsigned target1 = control & 63, target2 = (control >> 8) & 63;
    bool alpha = ((control >> 6) & 3) == 1 && target1 && target2 && ((Reg(0x52) >> 8) & 31);

    if (alpha)
        for (unsigned bg = 0; bg < 4; ++bg)
            if ((target1 & (1u << bg)) && (display & (0x100u << bg)))
                found |= SLOT_BG(Reg(8 + bg * 2) & 3);
    if (display & 0x1000)
        for (unsigned i = 0; i < 128; ++i)
        {
            unsigned attr0 = sMemory.oam[i * 4], mode = (attr0 >> 10) & 3;

            if (!(attr0 & 0x100) && (attr0 & 0x200)) continue;
            if (mode == 1 || (mode == 0 && alpha && (target1 & 16)))
                found |= SLOT_OBJ((sMemory.oam[i * 4 + 2] >> 10) & 3);
        }
    return found;
}

/*
 * Released again once unused for a few seconds - the slider lowered, or the
 * voxel overworld on screen, which never composes planes. They are 1.5 MiB of
 * VRAM, and kept for good after the title screen was shown in 3D they left
 * the overworld's atlases, pages and chunks starving for it.
 */
#define CTR_BANDS_IDLE_FRAMES 180
static uint32_t sBandsUsedFrame;
/* Asked by the overworld when an atlas found no VRAM; honoured before the next
 * frame opens, since deleting a render target may wait for the GPU. */
static bool sPlaneReleaseAsked;

void CtrVideo_RequestPlaneRelease(void)
{
    sPlaneReleaseAsked = true;
}

/*
 * Asked by a stage whose layer textures did not fit beside three planes: the
 * intro's four 256x512 layers (1 MiB) and three planes (768 KiB) are more than
 * an Old 3DS has free. Giving up the nearest plane is enough for them, and the
 * stage keeps two depths; giving up all of them had it composed per eye, and
 * at 30 fps.
 */
static bool sPlaneShrinkAsked;

/*
 * Set inside a frame that wanted the planes and found none. They are made
 * before the next frame opens: a failed attempt frees the planes it did get,
 * and C3D_RenderTargetDelete inside an open frame is svcBreak(USERBREAK_PANIC)
 * in citro3d - on an Old 3DS, whose VRAM often holds only two of the three,
 * that was opening a menu with the slider up. Until then, per eye.
 */
static bool sBandsWanted;

static void BandsRelease(void)
{
    for (unsigned i = 0; i < CTR_BANDS; ++i)
    {
        if (sBand[i]) C3D_RenderTargetDelete(sBand[i]);
        if (sBandTex[i].data) C3D_TexDelete(&sBandTex[i]);
        sBand[i] = NULL;
        memset(&sBandTex[i], 0, sizeof(sBandTex[i]));
    }
    sBandCount = 0;
    sBandsReady = false;
}

/* The last plane goes; outside the frame, like BandsRelease. */
static void BandsShrink(void)
{
    unsigned last = sBandCount - 1;

    C3D_RenderTargetDelete(sBand[last]);
    C3D_TexDelete(&sBandTex[last]);
    sBand[last] = NULL;
    memset(&sBandTex[last], 0, sizeof(sBandTex[last]));
    --sBandCount;
}

/* Allocated on first use: a console that never opens the 3D slider never pays
 * the VRAM. A failure here is not fatal, it just keeps the direct path. */
static bool BandsCreate(void);

/* In the frame: whether the planes are there, asking for them if not. */
static bool BandsUsable(void)
{
    sBandsUsedFrame = sStats.frames;
    if (!sBandsReady) sBandsWanted = true;
    return sBandsReady;
}

/* Outside the frame only; see sBandsWanted. */
static bool BandsReady(void)
{
    sBandsUsedFrame = sStats.frames;
    if (sBandsReady) return true;
    if (sBandsFailed && sStats.frames < sBandsRetryFrame) return false;
    for (unsigned attempt = 0; attempt < 3 && !sBandsReady; ++attempt)
    {
        if (BandsCreate())
            sBandsReady = true;
        /*
         * The overworld keeps an atlas per tileset pair it has met, and a
         * 2D screen - a menu, a battle - is where that VRAM is wanted back:
         * the atlases of maps not on screen go, the current map's stay.
         */
        else if (attempt == 0)
        {
#if CTR_VOXEL_ENABLED
            CtrVoxel_ReleaseIdleVram();
#endif
        }
        /*
         * Back from a battle, its scene surface is kept a few seconds in case
         * another one follows, and the field has taken its layer textures
         * back meanwhile: the planes did not fit beside both and stayed away,
         * flat. On the field the planes come first.
         */
        else if (attempt == 1 && sScene && !sBattle && !sTransition)
            SceneRelease();
        else
            break;
    }
    if (!sBandsReady)
    {
        if (!sBandsFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for 3D depth planes (free=%lu); composing "
                         "per eye, retrying later", (unsigned long)vramSpaceFree());
        sBandsFailed = true;
        sBandsRetryFrame = sStats.frames + CTR_BANDS_RETRY_FRAMES;
        return false;
    }
    sBandsFailed = false;
    CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes ready (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    return true;
}

/* The three planes and their targets, or nothing at all. */
static bool BandsCreate(void)
{
    for (unsigned i = 0; i < CTR_BANDS; ++i)
    {
        /* 5551: the GBA's own 15 bits and the one bit of cover a plane
         * needs, at half the VRAM of RGBA8 - 768 KiB for all three, which an
         * Old 3DS has beside the overworld's arenas; 1.5 MiB it did not. */
        if (!C3D_TexInitVRAM(&sBandTex[i], 512, 256, GPU_RGBA5551)) goto fail;
        C3D_TexSetFilter(&sBandTex[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sBandTex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        /* No depth buffer: Citro2D draws in submission order, and clearing one
         * per plane per frame is memory traffic this path exists to avoid. */
        sBand[i] = C3D_RenderTargetCreateFromTex(&sBandTex[i], GPU_TEXFACE_2D, 0, -1);
        if (!sBand[i]) goto fail;
        sBandCount = i + 1;
    }
    return true;
fail:
    /* Two planes still give the interface its depth (see BandsShrink). */
    if (sBandCount >= 2)
    {
        unsigned i = sBandCount;

        if (sBand[i]) C3D_RenderTargetDelete(sBand[i]);
        if (sBandTex[i].data) C3D_TexDelete(&sBandTex[i]);
        sBand[i] = NULL;
        memset(&sBandTex[i], 0, sizeof(sBandTex[i]));
        return true;
    }
    BandsRelease();
    return false;
}

/*
 * Plane i holds priority i on its own, except the last, which holds every
 * remaining priority so that anything blending with what is beneath it keeps
 * that beneath in the same surface. The last plane therefore always reaches
 * priority 3, sits at the screen plane and carries the backdrop; the others
 * start transparent and are stacked on top of it.
 */
static unsigned BandMask(unsigned band, unsigned count)
{
    (void)count;
    return sBandSlots[band];
}

static float BandDepth(unsigned band, unsigned count)
{
    return band + 1 < count ? (float)(CTR_PRIORITIES - 1 - band) : 0.0f;
}

/*
 * C2D_TargetClear for a plane. The GPU fills a 16-bit surface with the low
 * half of the value it is given, and C2D_TargetClear gives it the RGBA8 word,
 * whose low half is blue and alpha: opaque black came out blue, and any
 * backdrop some other colour. The colour is packed as RGBA5551 instead.
 */
static void PlaneClear(C3D_RenderTarget *target, uint32_t color)
{
    unsigned r = color & 255, g = (color >> 8) & 255, b = (color >> 16) & 255, a = color >> 24;

    C2D_Flush();
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL,
                          (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128), 0);
}

/* Composes every depth plane into its own surface, with no displacement. */
/*
 * Where in a plane of these slots something blends, in GBA coordinates: the
 * box round its blending sprites, or false when a background blends, or the
 * screen is one whose sprites this cannot place - then the whole view.
 */
static bool BlendBox(unsigned slots, int *x0, int *y0, int *x1, int *y1)
{
    static const uint8_t dimensions[3][4][2] = {
        {{8,8},{16,16},{32,32},{64,64}},
        {{16,8},{32,8},{32,16},{64,32}},
        {{8,16},{8,32},{16,32},{32,64}}
    };
    unsigned display = Reg(0), control = Reg(0x50);
    unsigned target1 = control & 63;
    bool alpha = ((control >> 6) & 3) == 1;

    if (!(sStage || sCentred) || (display & 0x6000)) return false;
    for (unsigned bg = 0; bg < 4 && alpha; ++bg)
        if ((target1 & (1u << bg)) && (display & (0x100u << bg)) && (slots & SLOT_BG(Reg(8 + bg * 2) & 3)))
            return false;
    *x0 = *y0 = 1 << 20;
    *x1 = *y1 = -(1 << 20);
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned attr0 = sMemory.oam[i * 4], attr1 = sMemory.oam[i * 4 + 1];
        unsigned mode = (attr0 >> 10) & 3, shape = attr0 >> 14;
        bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
        int x, y, w, h;

        if ((!affine && (attr0 & 0x200)) || shape == 3) continue;
        if (!(slots & SLOT_OBJ((sMemory.oam[i * 4 + 2] >> 10) & 3))) continue;
        if (!(mode == 1 || (mode == 0 && alpha && (target1 & 16)))) continue;
        w = dimensions[shape][attr1 >> 14][0] << twice;
        h = dimensions[shape][attr1 >> 14][1] << twice;
        x = (int)(attr1 & 511);
        y = (int)(attr0 & 255);
        if (x + w > 512) x -= 512;
        if (y + h > 256) y -= 256;
        if (x < *x0) *x0 = x;
        if (y < *y0) *y0 = y;
        if (x + w > *x1) *x1 = x + w;
        if (y + h > *y1) *y1 = y + h;
    }
    return *x0 < *x1;
}

/*
 * A plane whose layers blend needs what lies behind them to blend with, and
 * the planes behind it are other surfaces. So that plane is first given the
 * whole picture behind it - the backdrop and every further priority -
 * written to its colour but not its alpha, which stays clear: where its own
 * layers draw, they blend with the right colours and make the pixel opaque;
 * everywhere else it stays transparent and the plane behind shows at its own
 * depth. The logo over the intro's leaves, a sprite's shadow on the field,
 * keep their depth instead of flattening the frame to one plane.
 */
static void RenderBands(unsigned count, uint32_t backdrop)
{
    unsigned blending = BlendingPriorities();

    sParallax = 0;
    for (int band = (int)count - 1; band >= 0; --band)
    {
        unsigned mask = BandMask(band, count);
        bool last = band + 1 == (int)count, behind = !last && (blending & mask);
        uint32_t before;

        sLayerShift = 0;
        BlendForget();
        PlaneClear(sBand[band], last ? backdrop : behind ? backdrop & 0x00ffffff : 0);
        C2D_SceneBegin(sBand[band]);
        if (behind)
        {
            C2D_Flush();
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE);
            unsigned top = mask;

            while (top & (top - 1)) top &= top - 1;
            int x0, y0, x1, y1;

            /* Every slot behind this plane's. */
            sPriorityMask = SLOTS_ALL & ~((top << 1) - 1);
            Blend(5, false, false);
            StageUnderlay();
            /* Only under what blends: the logo, not the whole scene again. */
            if (BlendBox(mask, &x0, &y0, &x1, &y1))
            {
                if (x0 > sClipX0) sClipX0 = x0;
                if (y0 > sClipY0) sClipY0 = y0;
                if (x1 < sClipX1) sClipX1 = x1;
                if (y1 < sClipY1) sClipY1 = y1;
                C2D_Flush();
                Scissor(sClipX0, sClipY0, sClipX1, sClipY1);
                sScissored = true;
            }
            if (!(Reg(0) & 128) && sClipX0 < sClipX1 && sClipY0 < sClipY1) Compose();
            ClipToView();
            sScissored = false;
            C2D_Flush();
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            BlendForget();
        }
        before = sStats.tiles;
        sPriorityMask = mask;
        if (last)
        {
            Blend(5, false, false);
            StageUnderlay();
        }
        if (!(Reg(0) & 128)) Compose();
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        /* The furthest plane carries the backdrop, so it is never empty. */
        sBandUsed[band] = sStats.tiles != before || band + 1 == (int)count;
    }
    sPriorityMask = SLOTS_ALL;
    /* Rendering to a texture and then sampling it needs a command split. */
    C3D_FrameSplit(0);
}

/* Stacks the depth planes on one screen buffer, each displaced by its depth. */
static void BlitBands(C3D_RenderTarget *target, unsigned count, float parallax)
{
    const Tex3DS_SubTexture logical = {CTR_GAME_WIDTH, CTR_GAME_HEIGHT, 0, 1,
        CTR_GAME_WIDTH / 512.0f, 1 - CTR_GAME_HEIGHT / 256.0f};

    BlendForget();
    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(target);
    C2D_ViewReset();
    Blend(5, false, false);
    for (int band = (int)count - 1; band >= 0; --band)
        if (sBandUsed[band])
            C2D_DrawImageAt((C2D_Image){&sBandTex[band], &logical},
                            parallax * BandDepth(band, count), 0, 0, NULL, 1, 1);
    C2D_Flush();
}

#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
#if CTR_SHOW_FPS
/*
 * The FPS counter (SHOW_FPS=1), shown when the options turn it on
 * (CtrSettings_ShowFps): a 3x5 pixel font, each pixel a 1x1 solid
 * rectangle, over a translucent box in the top-left corner. Drawn last, over
 * whatever the frame composed, and at zero parallax in both eyes.
 */
#define FPS_PIXEL 1.0f
#define FPS_ADVANCE (4 * FPS_PIXEL)

static const char *GetHudGlyph(char c)
{
    switch (c)
    {
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "111001111100111";
    case '3': return "111001111001111";
    case '4': return "101101111001001";
    case '5': return "111100111001111";
    case '6': return "111100111101111";
    case '7': return "111001001001001";
    case '8': return "111101111101111";
    case '9': return "111101111001111";
    case 'A': case 'a': return "111101111101101";
    case 'B': case 'b': return "110101110101110";
    case 'C': case 'c': return "111100100100111";
    case 'D': case 'd': return "110101101101110";
    case 'E': case 'e': return "111100111100111";
    case 'F': case 'f': return "111100110100100";
    case 'G': case 'g': return "111100101101111";
    case 'H': case 'h': return "101101111101101";
    case 'I': case 'i': return "111010010010111";
    case 'J': case 'j': return "001001001101111";
    case 'K': case 'k': return "101101110101101";
    case 'L': case 'l': return "100100100100111";
    case 'M': case 'm': return "101111101101101";
    case 'N': case 'n': return "111101101101101";
    case 'O': case 'o': return "111101101101111";
    case 'P': case 'p': return "111101111100100";
    case 'Q': case 'q': return "111101101111001";
    case 'R': case 'r': return "110101110101101";
    case 'S': case 's': return "111100111001111";
    case 'T': case 't': return "111010010010010";
    case 'U': case 'u': return "101101101101111";
    case 'V': case 'v': return "101101101101010";
    case 'W': case 'w': return "101101101111101";
    case 'X': case 'x': return "101101010101101";
    case 'Y': case 'y': return "101101010010010";
    case 'Z': case 'z': return "111001010100111";
    case ':': return "000010000010000";
    case '+': return "000010111010000";
    case '-': return "000000111000000";
    case '[': return "110100100100110";
    case ']': return "011001001001011";
    default: return NULL;
    }
}

static void DrawFps(C3D_RenderTarget *target)
{
    unsigned fps = (unsigned)(sStats.fps + 0.5f);
    bool ff = CtrPlatform_GetFastForward();
    char line1[32];
    snprintf(line1, sizeof(line1), "FPS: %u [FF:%s]", fps > 999 ? 999 : fps, ff ? "2X" : "1X");
    const char *line2 = "Y:VOXEL  R+Y:3D";
    const char *line3 = "R+START:FF  L+Y:HUD";

    const char *lines[3] = { line1, line2, line3 };
    int numLines = 3;

    int maxLen = 0;
    for (int l = 0; l < numLines; ++l)
    {
        int len = (int)strlen(lines[l]);
        if (len > maxLen) maxLen = len;
    }

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    C2D_SceneBegin(target);
    C2D_ViewReset();
    Blend(5, false, false);

    float boxW = maxLen * FPS_ADVANCE + FPS_PIXEL * 4;
    float boxH = numLines * 7.0f * FPS_PIXEL + FPS_PIXEL * 3;
    C2D_DrawRectSolid(2, 2, 0, boxW, boxH, C2D_Color32(0, 0, 0, 160));

    for (int l = 0; l < numLines; ++l)
    {
        const char *str = lines[l];
        float startY = 2 + FPS_PIXEL * 2 + l * 7.0f * FPS_PIXEL;
        for (int i = 0; str[i]; ++i)
        {
            const char *bits = GetHudGlyph(str[i]);
            if (!bits) continue;
            float startX = 2 + FPS_PIXEL * 2 + i * FPS_ADVANCE;
            for (unsigned p = 0; p < 15; ++p)
            {
                if (bits[p] == '1')
                {
                    C2D_DrawRectSolid(startX + (p % 3) * FPS_PIXEL,
                                      startY + (p / 3) * FPS_PIXEL,
                                      0, FPS_PIXEL, FPS_PIXEL,
                                      C2D_Color32(255, 255, 255, 255));
                }
            }
        }
    }
    C2D_Flush();
}
#endif

/*
 * The bottom screen runs some of the game's menus without showing them (the
 * party menu, the bag): while it does, the top screen keeps the last frame it
 * presented, the world as it was when the menu opened. Nothing is rendered;
 * the frame is only paced to the display.
 */
static bool sHoldTop;

void CtrVideo_HoldTop(bool hold)
{
    if (hold != sHoldTop)
        CtrLog_Write(CTR_LOG_VIDEO, "top screen %s at frame %lu", hold ? "held" : "released",
                     (unsigned long)sStats.frames);
    sHoldTop = hold;
}

void CtrVideo_Present(void)
{
    uint64_t entry = svcGetSystemTick();
    (void)entry;

    if (!sMemory.regs) CtrPlatform_Fatal("VIDEO has no logical memory bound");
    /* The PokéNav, the PC's boxes and the bag are drawn on the bottom screen, whether
     * or not the top is held. */
    bool bottom = BottomReady(BottomScreen(sCentredRequested) && !sStageRequested);
    sBottomInUse = bottom;
    if (sHoldTop && !bottom)
    {
        gspWaitForVBlank();
        ++sStats.frames;
        return;
    }
    /* Wait for previous GPU work before editing the atlas. C3D owns VBlank
     * pacing and swap: no gfxSwapBuffers/gspWaitForVBlank in this path. */
    /* Outside the frame, where deleting a render target may wait for the GPU. */
    /* A battle transition is flat and needs the battle scene's surface, which
     * the planes' VRAM is what keeps out on the 2D field: they go first. */
    if (sBandsReady && (sPlaneReleaseAsked || sTransitionRequested
                        || sStats.frames - sBandsUsedFrame > CTR_BANDS_IDLE_FRAMES))
    {
        BandsRelease();
        CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes released (VRAM free=%lu)",
                     (unsigned long)vramSpaceFree());
    }
    else if (sBandsReady && sPlaneShrinkAsked && sBandCount > 2)
    {
        BandsShrink();
        CtrLog_Write(CTR_LOG_VIDEO, "3D depth planes: %u, the rest to a stage layer (VRAM free=%lu)",
                     sBandCount, (unsigned long)vramSpaceFree());
    }
    sPlaneReleaseAsked = sPlaneShrinkAsked = false;
    if (sLeavesTex[0].data && sStats.frames - sLeavesUsed > 120) LeavesRelease();
    if (sBandsWanted && !sTransitionRequested)
    {
        sBandsWanted = false;
        BandsReady();
    }
    /*
     * The view of this frame. The voxel overworld is never a stage, so a stage
     * only waits on the voxel decision in the unlikely case both are asked.
     */
    sStage = sStageRequested;
    sCentred = sCentredRequested != CTR_CENTRED_NONE && !sStage;
    sCentredScreen = sCentred ? sCentredRequested : CTR_CENTRED_NONE;
    sBattle = sBattleRequested && !sStage && !sCentred;
    sTransition = sTransitionRequested && sLineRegs && !sStage && !sCentred && !sBattle;
    sZoom = sBattle ? CTR_BATTLE_ZOOM : 1.0f;
    /* GBA (120, 112) - the middle of the scene's bottom edge - on screen (200, 192). */
    sOffX = sBattle ? CTR_GAME_WIDTH / 2 - 120 * sZoom : 0.0f;
    sOffY = sBattle ? CTR_GAME_HEIGHT - 48 - 112 * sZoom : 0.0f;
    sShiftZoom = sZoom;
    if (sStage || sCentred)
    {
        sViewX = CTR_STAGE_X;
        /* The PokeNav is on the bottom screen, laid out band by band from
         * its top edge (NavCompose). */
        sViewY = sCentredScreen == CTR_CENTRED_POKENAV ? 0 : CTR_STAGE_Y;
    }
    else if (sBattle)
    {
        sViewX = sViewY = 0;
    }
    else
    {
        sViewX = sViewY = 0;
    }
    ClipToView();
    /* The voxel overworld draws the field itself when it is switched on. */
    sFieldLayers = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld() && !CtrSettings_Voxel();
    LayersPrepare();
    ScenePrepare();
    uint64_t waitStart = svcGetSystemTick();
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    sUploadCommands = 0;
    sRenderReserve = sBattle ? 28u : 24u;
    uint64_t start = svcGetSystemTick();
    sStats.waitMs = (start - waitStart) * 1000.0 / SYSCLOCK_ARM11;
#if CTR_VOXEL_ENABLED
    /*
     * FrameBegin returns on a VBlank, so the time between two returns is a
     * whole number of display frames: two of them is a frame the screen
     * showed twice. Logged with what the frame before it spent - the present
     * (and the voxel builds in it), then the game and its VBlank handler - so
     * that a slow renderer can be told from a slow game on hardware.
     */
    {
        static uint64_t sLastBegin;
        static unsigned sDropsLogged;
        /* A screen that runs at 30 for a while is one finding, not a line per
         * frame: a steady run is logged once a second with how many frames it
         * dropped, and only a hitch of three frames or more is always logged. */
        static uint32_t sLastDropLogged;
        static unsigned sDropsQuiet;
        float gap = sLastBegin ? (start - sLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
        /*
         * Which of the two missed the VBlank: the CPU, if it came back to
         * FrameBegin after it (`arrive` past 16.7), or else the GPU, whose
         * last frame ended `gpuEnd` after that frame began - its FrameEnd,
         * the present, plus the drawing, which FrameBegin has waited for.
         */
        float arrive = sLastBegin ? (waitStart - sLastBegin) * 1000.0f / SYSCLOCK_ARM11 : 0.0f;
        float gpuEnd = sStats.cpuMs + C3D_GetDrawingTime();

        if (gap > 24.0f && sStats.frames > 120 && sDropsLogged < 3000)
        {
            if (gap < 45.0f && sStats.frames - sLastDropLogged < 60)
                ++sDropsQuiet;
            else
            {
                const CtrTiming *timing = CtrPlatform_GetTiming();
                const CtrVoxelStats *voxel = CtrVoxel_GetStats();
                float logMs, logAgo;

                CtrLog_LastDrain(&logMs, &logAgo);

                ++sDropsLogged;
                sLastDropLogged = sStats.frames;
                /* The voxel figures are the last voxel frame's: on a 2D frame they
                 * are stale, and present= alone is the 2D compositor. */
                CtrLog_Write(CTR_LOG_VIDEO, "DROP frame=%lu gap=%.1fms (+%u quiet): present=%.1f "
                             "(voxel=%.1f: world=%.1f atlas=%.1f chunks=%.1f sprites=%.1f "
                             "stream=%.1f anim=%.1f draft=%.1f) "
                             "after=%.1f/%.1f gpu=%.1f game=%.1f audio+vblank=%.1f "
                             "arrive=%.1f gpuEnd=%.1f%s bottom=%.1f pre=%.1f log=%.1f@%.0f",
                             (unsigned long)sStats.frames, gap, sDropsQuiet, sStats.cpuMs,
                             voxel->updateMs, voxel->worldMs, voxel->atlasMs,
                             voxel->meshMs - voxel->atlasMs, voxel->spritesMs,
                             voxel->streamMs, voxel->animMs, voxel->draftMs,
                             voxel->afterMs, voxel->afterBudgetMs, sStats.gpuMs,
                             timing->gameMs, timing->vblankMs, arrive, gpuEnd,
                             arrive > 17.0f ? " (cpu late)" : gpuEnd > 15.5f ? " (gpu late)" : "",
                             timing->bottomMs, (waitStart - entry) * 1000.0f / SYSCLOCK_ARM11,
                             logMs, logAgo);
                sDropsQuiet = 0;
            }
        }
        sLastBegin = start;
    }
#endif
    sStats.tiles = sStats.uploads = sStats.sprites = 0;
    sBgTicks = sObjTicks = 0;
    sStats.display = Reg(0);
    if (sUsed > CACHE_COUNT - 4096) { memset(sHash, 0, sizeof(sHash)); sUsed = 0; }
    UpdatePalette();
    if (sStage || sBattle) RecordScroll();
    /* The shown backdrop, faded: sPalette may hold the unfaded one. */
    uint16_t backdrop = (Reg(0) & 128) ? 0x7fff : sMemory.palette[0] & 0x7fff;
    uint32_t rgb = CtrVideo_RGBA8(backdrop, true);
    uint32_t clear = C2D_Color32(rgb >> 24, rgb >> 16, rgb >> 8, 255);
    /*
     * The 3D slider decides the separation, and at zero the right eye is not
     * composed at all: with 3D off this is the same single pass as before.
     * Whole pixels only, so every layer stays on the pixel grid in both eyes.
     */
    bool field = !sStage && !sCentred && !sBattle && CtrGame_IsOverworld();
#if CTR_VOXEL_ENABLED
    /*
     * Preparing the voxel frame is part of the decision. If the atlas or the
     * mesh could not be built this frame, the 2D compositor draws it: leaving
     * the previous map's geometry on screen would show the wrong place.
     */
    /* The overworld, drawn in 3D or - while its first atlas or mesh is still
     * being made - by the 2D compositor. Either way it never takes the depth
     * planes: holding them there is what kept the overworld's atlas out of
     * VRAM for good, the 2D picture standing in for it frame after frame. */
    /* Opt-in from the bottom screen's options (CtrSettings_Voxel). */
    bool overworld = field && CtrSettings_Voxel() && CtrVoxel_IsAvailable();
    bool voxel = overworld && CtrVoxel_Update();
    /* A new map still being made - a frame or two, behind the fade - is
     * black rather than the 2D picture flashing up before the 3D one. */
    bool blank = overworld && !voxel && CtrVoxel_IsWarmingUp();
#else
    const bool voxel = false, overworld = false, blank = false;
#endif
    /*
     * A battle in front of the voxel world: the world updated as the battle's
     * scenery whenever the option is on, and drawn as it while BG3 shows the
     * scenery - a move's background on BG3 is the 2D battle's, whole.
     */
    sBattleWorld = false;
    sWorldLayers = 0;
#if CTR_VOXEL_ENABLED
    bool battleUpdated = false;
    if (sBattle && CtrSettings_Voxel() && CtrSettings_VoxelBattle() && CtrVoxel_IsAvailableForBattle())
    {
        bool sliding = CtrBattleIntro_Sliding() != 0, moveBg = CtrBattleBg_MoveBgShown() != 0;

        if (!CtrVoxel_InBattle())
            CtrVoxel_BeginBattle();
        /* The intro slides the scenery in line by line: no shake there. */
        CtrVoxel_SetBattleFrame(sliding, sliding || moveBg ? 0.0f : BattleScenerySway(0x1c),
                                sliding || moveBg ? 0.0f : BattleScenerySway(0x1e));
        battleUpdated = CtrVoxel_Update();
        sBattleWorld = battleUpdated && !moveBg && (Reg(0) & 0x800) && !(Reg(0) & 128);
        if (sBattleWorld)
            sWorldLayers = (1u << 3) | (sliding && !VoxelBattle_IsLink() ? (1u << 1) | (1u << 2) : 0u);
    }
#endif
    /* The 2D field centres its text windows as the voxel overlay does. */
    sFieldUi = field && !voxel;
    float slider = osGet3DSliderState();
    /* Real stereoscopy for 2D layers and 3D voxel world (opt-in on Old 3DS for performance). */
    bool stereo = !blank && !sBattleWorld && !sTransition && sTopRight && slider > 0.0f
                  && ((voxel && CtrSettings_VoxelStereo()) || (!voxel && roundf(slider * CTR_STEREO_PIXELS) > 0.0f));
    /* A 2D screen composed per eye walks every layer twice, which on an Old
     * 3DS is 30 fps in a menu. Without its planes it stays flat until they
     * can be made (before the next frame, see sBandsWanted). */
    /* The battle scene is two passes of its own (RenderBattleScene), and cheap
     * enough to be composed per eye. A stage is not: the intro and the title
     * draw their waves line by line, and twice that is more than an Old 3DS
     * has in a frame, so they take the planes like any other 2D screen. */
    if (!sStage) sStageWithoutPlanes = false;
    bool planes = stereo && !overworld && !sBattle && !sStageWithoutPlanes && BandsUsable();
    if (stereo && !overworld && !sStage && !sBattle && !planes) stereo = false;
    if (bottom) stereo = planes = false;
    if (stereo != sStereo) { gfxSet3D(stereo); sStereo = stereo; }
    sStats.stereo = stereo ? roundf(slider * CTR_STEREO_PIXELS) : 0;
    if (!voxel && !blank) LayersRender();

    if (bottom)
    {
        /* The top screen is not drawn: it keeps the frame it showed last. */
        sPlanes = 0;
        RenderEye(sBottom, clear, 0.0f);
    }
    else if (sTransition && !blank)
    {
        sPlanes = 0;
        RenderTransition(voxel, clear);
    }
    else if (voxel)
    {
#if CTR_VOXEL_ENABLED
        sPlanes = 0;
        RenderVoxel(clear, stereo ? slider : 0.0f);
#endif
    }
    else if (blank)
    {
        sPlanes = 0;
        C2D_TargetClear(sTop, C2D_Color32(0, 0, 0, 255));
    }
#if CTR_VOXEL_ENABLED
    else if (sBattleWorld)
    {
        sPlanes = 0;
        RenderBattleWorld(clear);
    }
#endif
    else if (!stereo)
    {
        sPlanes = 0;
        RenderEye(sTop, clear, 0.0f);
    }
    else if (planes)
    {
        sPlanes = DepthPlanes();
        RenderBands(sPlanes, clear);
        BlitBands(sTop, sPlanes, sStats.stereo);
        BlitBands(sTopRight, sPlanes, -(float)sStats.stereo);
    }
    else
    {
        /* The battle, or a stage without memory for its planes. */
        sPlanes = 0;
        RenderEye(sTop, clear, sStats.stereo);
        RenderEye(sTopRight, clear, -(float)sStats.stereo);
    }
#if CTR_SHOW_FPS
    if (CtrSettings_ShowFps())
    {
        if (!bottom) DrawFps(sTop);
        if (stereo) DrawFps(sTopRight);
    }
#endif
    /* Queued in the frame, behind the drawing into sBottom (BottomTransfer). */
    if (bottom) BottomTransfer();
    C3D_FrameEnd(0);
    ++sStats.frames;
    ++sFpsFrames;
    sStats.cpuMs = (svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11;
    sStats.gpuMs = C3D_GetDrawingTime();
#if CTR_VOXEL_ENABLED
    /* The GPU draws the frame now: the voxel world builds what comes next in
     * the time the CPU would otherwise wait for the VBlank. */
    if (overworld || battleUpdated)
        CtrVoxel_AfterSubmit(start);
#endif
    uint64_t now = CtrPlatform_Milliseconds();
    if (now - sFpsStart >= 1000)
    {
        sStats.fps = sFpsFrames * 1000.0f / (now - sFpsStart);
        sFpsStart = now;
        sFpsFrames = 0;
    }
    if (sStats.frames % 600 == 0)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "frames=%lu fps=%.1f cpu=%.2fms bg=%.2fms obj=%.2fms gpu=%.2fms "
                     "3d=%lupx%s x%lu quads=%lu sprites=%lu errors=%lu cache=%u linear=%lu vram=%lu",
                     (unsigned long)sStats.frames, sStats.fps, sStats.cpuMs,
                     sBgTicks * 1000.0 / SYSCLOCK_ARM11, sObjTicks * 1000.0 / SYSCLOCK_ARM11,
                     sStats.gpuMs, (unsigned long)sStats.stereo,
                     sStats.stereo ? (sPlanes ? "/planes" : "/eyes") : "", (unsigned long)sPlanes,
                     (unsigned long)sStats.tiles, (unsigned long)sStats.sprites,
                     (unsigned long)sStats.errors, sUsed,
                     (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());
#if CTR_VOXEL_ENABLED
        if (voxel || sBattleWorld)
        {
            const CtrVoxelStats *stats = CtrVoxel_GetStats();
            CtrLog_Write(CTR_LOG_VIDEO,
                         "VOXEL chunks=%u/%u missing=%u pending=%u builds=%u atlas=%u verts=%u "
                         "mesh=%.2fms peak=%.2fms update=%.2fms peak=%.2fms dropped=%u "
                         "anim=%u/%u refl=%u drafts=%u/%u",
                         stats->visibleChunks, stats->chunks, stats->chunksMissing,
                         stats->pendingBuilds, stats->meshRebuilds, stats->atlasRebuilds,
                         stats->vertices, stats->meshMs, stats->meshPeakMs,
                         stats->updateMs, stats->updatePeakMs, stats->dropped,
                         stats->animationUploads, stats->animatedMetatiles,
                         stats->reflections, stats->draftsVisible, stats->draftsMade);
        }
#endif
    }
}
