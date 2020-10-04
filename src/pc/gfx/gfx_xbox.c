#ifdef TARGET_XBOX

#define SHOW_FPS 0
#define WIREFRAME 0

#include <time.h>
#include <errno.h>
#include <assert.h>
#include <PR/gbi.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <math.h>
#include <pbkit/pbkit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xboxkrnl/xboxkrnl.h>
#include <hal/debug.h>
#include <windows.h>

#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_cc.h"
#include "gfx_xbox_swizzle.h"

// FIXME: Move these defs out
#define MASK(mask, val) (((val) << (__builtin_ffs(mask)-1)) & (mask))
#define NV2A_VERTEX_ATTR_DIFFUSE        3
#define NV2A_VERTEX_ATTR_SPECULAR       4
#define NV2A_VERTEX_ATTR_TEXTURE0       9
#define NV2A_VERTEX_ATTR_TEXTURE1       10
#define NV097_SET_SPECULAR_ENABLE 0x000003b8
#define MAX_Z 16777215.0
#define MAXRAM 0x03FFAFFF

enum PS_REGISTER
{
    PS_REGISTER_ZERO=              0x00L, // r
    PS_REGISTER_DISCARD=           0x00L, // w
    PS_REGISTER_C0=                0x01L, // r
    PS_REGISTER_C1=                0x02L, // r
    PS_REGISTER_FOG=               0x03L, // r
    PS_REGISTER_V0=                0x04L, // r/w
    PS_REGISTER_V1=                0x05L, // r/w
    PS_REGISTER_T0=                0x08L, // r/w
    PS_REGISTER_T1=                0x09L, // r/w
    PS_REGISTER_T2=                0x0aL, // r/w
    PS_REGISTER_T3=                0x0bL, // r/w
    PS_REGISTER_R0=                0x0cL, // r/w
    PS_REGISTER_R1=                0x0dL, // r/w
    PS_REGISTER_V1R0_SUM=          0x0eL, // r
    PS_REGISTER_EF_PROD=           0x0fL, // r
};

static bool is_pow_2(int x)
{
    return (x != 0) && ((x & (x - 1)) == 0);
}

// XXX: Updated by controller logic
int g_xbox_exit_button_state;

static int g_width, g_height;
#if SHOW_FPS
static int g_start, g_last, g_now;
static int g_fps, g_frames, g_frames_total;
#endif

#define SHADER_POOL_SIZE 64

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc_features;
    int num_textures;
    uint8_t num_floats;
    bool do_fog;
};

static struct ShaderProgram g_shader_pool[SHADER_POOL_SIZE];
static uint8_t g_shader_cnt = 0;
static struct ShaderProgram *g_cur_shader;

#define TEX_POOL_SIZE 512
#define SWIZZLE_BUF_SIDE_LEN 256

struct texture {
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint32_t cms;
    uint32_t cmt;
    bool linear_filter;
    void *addr;
    bool swizzled;
};

static struct texture g_tex_pool[TEX_POOL_SIZE];
static int g_tex_cnt = 0;
static int g_tex_bindings[2];
static int g_last_tile_selected;
static uint32_t *g_swizzle_buf;

// FIXME: Does not belong here
static uint32_t *pb_push2f(
    uint32_t *p, DWORD command, float param1, float param2)
{
    pb_push_to(SUBCH_3D,p,command,2);
    *(float*)(p+1)=param1;
    *(float*)(p+2)=param2;
    return p+3;
}

static void matrix_identity(float out[4][4])
{
    memset(out, 0, 4*4*sizeof(float));
    out[0][0] = 1.0f;
    out[1][1] = 1.0f;
    out[2][2] = 1.0f;
    out[3][3] = 1.0f;
}

static void matrix_viewport(
    float out[4][4],
    float x,
    float y,
    float width,
    float height)
{
    memset(out, 0, 4*4*sizeof(float));
    out[0][0] = width/2.0f;
    out[1][1] = height/-2.0f;
    out[2][2] = MAX_Z;
    out[3][3] = 1.0f;
    out[3][0] = x + width/2.0f;
    out[3][1] = y + height/2.0f;
}




static void gfx_xbox_wm_init(const char *game_name, bool start_in_fullscreen)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
}

static void gfx_xbox_wm_set_keyboard_callbacks(
    bool (*on_key_down)(int scancode),
    bool (*on_key_up)(int scancode),
    void (*on_all_keys_up)(void))
{
}

static void gfx_xbox_wm_set_fullscreen_changed_callback(
    void (*on_fullscreen_changed)(bool is_now_fullscreen))
{
}

static void gfx_xbox_wm_set_fullscreen(bool enable)
{
}

static void gfx_xbox_wm_main_loop(void (*run_one_game_iter)(void))
{
    int exit_button_pressed_time = 0;

    while (1) {
        // Allow user to exit to dash if they hold the back button for 2s
        if (g_xbox_exit_button_state) {
            int now = GetTickCount();
            if (exit_button_pressed_time == 0) {
                exit_button_pressed_time = now;
            }
            if ((now-exit_button_pressed_time) > 2000) {
                XReboot();
            }
        } else {
            exit_button_pressed_time = 0;
        }

        run_one_game_iter();
    }
}

static void gfx_xbox_wm_get_dimensions(uint32_t *width, uint32_t *height)
{
    *width = g_width;
    *height = g_height;
}

static void gfx_xbox_wm_handle_events(void)
{
}

static bool gfx_xbox_wm_start_frame(void)
{
    return true;
}

static void gfx_xbox_wm_swap_buffers_begin(void)
{
}

static struct timespec gfx_xbox_wm_timediff(
    struct timespec t1,
    struct timespec t2)
{
    t1.tv_sec -= t2.tv_sec;
    t1.tv_nsec -= t2.tv_nsec;
    if (t1.tv_nsec < 0) {
        t1.tv_nsec += 1000000000;
        t1.tv_sec -= 1;
    }
    return t1;
}

static struct timespec gfx_xbox_wm_timeadd(
    struct timespec t1,
    struct timespec t2)
{
    t1.tv_sec += t2.tv_sec;
    t1.tv_nsec += t2.tv_nsec;
    if (t1.tv_nsec > 1000000000) {
        t1.tv_nsec -= 1000000000;
        t1.tv_sec += 1;
    }
    return t1;
}

static void gfx_xbox_wm_swap_buffers_end(void)
{
    // XXX: Waiting for 2 VBL to run at 30Hz.  This could be nicer, but it
    // works well enough
    pb_wait_for_vbl();
    pb_wait_for_vbl();
}

static double gfx_xbox_wm_get_time(void)
{
    return 0.0;
}




static bool gfx_xbox_renderer_z_is_from_0_to_1(void)
{
    return true;
}

static void gfx_xbox_renderer_unload_shader(struct ShaderProgram *old_prg)
{
}

int map_shader_item_to_psh_input(int item)
{
    switch (item) {
    case SHADER_0:       return PS_REGISTER_ZERO;
    case SHADER_INPUT_1: return PS_REGISTER_V0;
    case SHADER_INPUT_2: return PS_REGISTER_V1;
    // case SHADER_INPUT_3:
    // case SHADER_INPUT_4:
    case SHADER_TEXEL0:  return PS_REGISTER_T0;
    case SHADER_TEXEL0A: return PS_REGISTER_T0;
    case SHADER_TEXEL1:  return g_cur_shader->num_textures > 1 ?
                                    PS_REGISTER_T1 :
                                    PS_REGISTER_T0;
    default:
        assert(0);
        return PS_REGISTER_ZERO;
    }
}

static void gfx_xbox_renderer_load_shader(struct ShaderProgram *new_prg)
{
    g_cur_shader = new_prg;
    struct CCFeatures *cc_feat = &g_cur_shader->cc_features;

    int in_0 = map_shader_item_to_psh_input(cc_feat->c[0][0]);
    int in_1 = map_shader_item_to_psh_input(cc_feat->c[0][1]);
    int in_2 = map_shader_item_to_psh_input(cc_feat->c[0][2]);
    int in_3 = map_shader_item_to_psh_input(cc_feat->c[0][3]);

    int in_0a = map_shader_item_to_psh_input(cc_feat->c[1][0]);
    int in_1a = map_shader_item_to_psh_input(cc_feat->c[1][1]);
    int in_2a = map_shader_item_to_psh_input(cc_feat->c[1][2]);
    int in_3a = map_shader_item_to_psh_input(cc_feat->c[1][3]);

    if (!cc_feat->opt_alpha) {
        in_0a = in_0;
        in_1a = in_1;
        in_2a = in_2;
        in_3a = in_3;
    }

    bool in_0a_c = false;
    bool in_1a_c = false;
    bool in_2a_c = (cc_feat->c[0][2] == SHADER_TEXEL0A);
    bool in_3a_c = false;

    int in_fog = 0;
    if (g_cur_shader->do_fog) {
        assert(cc_feat->num_inputs < 2);
        in_fog = PS_REGISTER_V0 + cc_feat->num_inputs;
    }

    uint32_t *p = pb_begin();
    pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT,
        MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE1, 0)
        | MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE2, 0)
        | MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE3, 0));
    p += 2;
    pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
        MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, g_cur_shader->num_textures > 0 ? NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE : NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_PROGRAM_NONE)
        | MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE1, g_cur_shader->num_textures > 1 ? NV097_SET_SHADER_STAGE_PROGRAM_STAGE1_2D_PROJECTIVE : NV097_SET_SHADER_STAGE_PROGRAM_STAGE1_PROGRAM_NONE)
        | MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE2, NV097_SET_SHADER_STAGE_PROGRAM_STAGE2_PROGRAM_NONE)
        | MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE3, NV097_SET_SHADER_STAGE_PROGRAM_STAGE3_PROGRAM_NONE));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
        MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, in_0) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, in_0a_c) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x1)
        | MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, in_1) | MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, in_1a_c) | MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x7)
        | MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x1));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4,
        MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, 0x0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST, 0x0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, PS_REGISTER_R1)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, in_0a) | MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x1)
        | MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, in_1a) | MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x7)
        | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x1));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 0 * 4,
        MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, 0x0)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST, 0x0)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, PS_REGISTER_R1)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 1 * 4,
        MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, PS_REGISTER_R1) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, in_2) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, in_2a_c) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, in_3) | MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, in_3a_c) | MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x1));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 1 * 4,
        MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, 0x0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST, 0x0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, PS_REGISTER_R0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 1 * 4,
        MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, PS_REGISTER_R1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, in_2a) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, in_3a) | MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x6)
        | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x1));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 1 * 4,
        MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, 0x0)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST, 0x0)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, PS_REGISTER_R0)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_CONTROL,
        MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 2));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,
        MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE, in_fog) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA, 1) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE, in_fog) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA, 0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE, PS_REGISTER_R0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA, 0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA, 0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,
        MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA, 0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE, 0x0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA, 0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, PS_REGISTER_R0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0));
    p += 2;

    // FIXME: Noise

    if (cc_feat->opt_texture_edge && cc_feat->opt_alpha) {
        p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, true);
        p = pb_push1(p, NV097_SET_ALPHA_FUNC, 0x204);
        p = pb_push1(p, NV097_SET_ALPHA_REF,  77);

    } else {
        p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, false);
        p = pb_push1(p, NV097_SET_ALPHA_FUNC, 0x207);
    }
    pb_end(p);
}

static struct ShaderProgram *gfx_xbox_renderer_create_and_load_new_shader(
    uint32_t shader_id)
{
    assert(g_shader_cnt < SHADER_POOL_SIZE);
    struct ShaderProgram *prg = &g_shader_pool[g_shader_cnt++];

    prg->shader_id = shader_id;
    gfx_cc_get_features(prg->shader_id, &prg->cc_features);

    prg->num_textures = prg->cc_features.used_textures[0]
                        + prg->cc_features.used_textures[1];
    // If we are using only 1 tile, make sure its tile 0
    if (prg->cc_features.used_textures[1]) {
        assert(prg->cc_features.used_textures[0]);
    }

    size_t num_floats = 4;
    if (prg->cc_features.used_textures[0]
        || prg->cc_features.used_textures[1]) {
        num_floats += 2;
    }
    if (prg->cc_features.opt_fog) {
        num_floats += 4;
    }
    for (int i = 0; i < prg->cc_features.num_inputs; i++) {
        num_floats += prg->cc_features.opt_alpha ? 4 : 3;
    }

    prg->num_floats = num_floats;

    // Only handle fog if we can passthru in one of the vertex color outputs
    prg->do_fog = (prg->cc_features.num_inputs < 2)
                  && prg->cc_features.opt_fog;

    gfx_xbox_renderer_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_xbox_renderer_lookup_shader(uint32_t shader_id)
{
    for (size_t i = 0; i < g_shader_cnt; i++) {
        if (g_shader_pool[i].shader_id == shader_id) {
            return &g_shader_pool[i];
        }
    }
    return NULL;
}

static void gfx_xbox_renderer_shader_get_info(
    struct ShaderProgram *prg,
    uint8_t *num_inputs,
    bool used_textures[2])
{
    *num_inputs = prg->cc_features.num_inputs;
    used_textures[0] = prg->cc_features.used_textures[0];
    used_textures[1] = prg->cc_features.used_textures[1];
}

static uint32_t gfx_xbox_renderer_new_texture(void)
{
    assert(g_tex_cnt < TEX_POOL_SIZE);

    struct texture *tex = &g_tex_pool[g_tex_cnt];
    tex->addr = NULL;
    tex->width = 0;
    tex->height = 0;
    tex->pitch = 0;
    tex->linear_filter = false;
    tex->cms = G_TX_CLAMP;
    tex->cmt = G_TX_CLAMP;
    tex->swizzled = false;

    return g_tex_cnt++;
}

static uint32_t gfx_cm_to_nv2a(uint32_t val)
{
    if (val & G_TX_CLAMP) {
        return 3;
    }
    return (val & G_TX_MIRROR) ? 2 : 1;
}

static void gfx_xbox_renderer_select_texture(
    int tile,
    uint32_t tex_id)
{
    assert(tile < 2);
    g_tex_bindings[tile] = tex_id;
    g_last_tile_selected = tile;

    struct texture *tex = &g_tex_pool[tex_id];
    if (tex->addr == NULL) {
        return;
    }

    uint32_t fmt;
    if (tex->swizzled) {
        int log_w = ffs(tex->width)-1;
        int log_h = ffs(tex->height)-1;
        fmt = 0x0001062a;
        fmt |= log_w << 20;
        fmt |= log_h << 24;
    } else {
        fmt = 0x0001122a;
    }

    uint32_t kill = (g_cur_shader->cc_features.opt_texture_edge
                     && g_cur_shader->cc_features.opt_alpha) ? (1 << 2) : 0;
    uint32_t address;
    if (tex->swizzled) {
        // Allow texture address (wrap/mirror/clamp)
        address = (gfx_cm_to_nv2a(tex->cms) << 0)
                | (gfx_cm_to_nv2a(tex->cmt) << 8)
                | (3 << 16);
    } else {
        // Can only clamp
        address = 0x030303;
    }

    uint32_t filter = tex->linear_filter ? 0x02072000 : 0x01014000;
    uint32_t offset = (DWORD)tex->addr & 0x03ffffff;

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(tile),
                    0x40000000|kill);
    p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(tile),
                    offset,
                    fmt);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(tile),
                    tex->pitch<<16);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(tile),
                    (tex->width<<16)|tex->height);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_MATRIX_ENABLE(tile),
                    0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(tile),
                    address);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(tile),
                    filter);
    pb_end(p);
}

static void gfx_xbox_renderer_upload_texture(
    const uint8_t *rgba32_buf,
    int width,
    int height)
{
    assert(g_last_tile_selected < 2);
    struct texture *tex = &g_tex_pool[g_tex_bindings[g_last_tile_selected]];
    tex->width = width;
    tex->height = height;
    tex->pitch = tex->width * 4;
    if (tex->addr != NULL) {
        MmFreeContiguousMemory(tex->addr);
    }
    tex->addr = MmAllocateContiguousMemoryEx(
        tex->pitch*tex->height, 0, MAXRAM, 0, 0x404);
    assert(tex->addr != NULL);

    tex->swizzled = is_pow_2(tex->width) && is_pow_2(tex->height);
    assert(!tex->swizzled || (
           (tex->width <= SWIZZLE_BUF_SIDE_LEN)
           && (tex->height <= SWIZZLE_BUF_SIDE_LEN)));

    uint32_t *in = (uint32_t *)rgba32_buf;
    uint32_t *out = tex->swizzled ? g_swizzle_buf : tex->addr;
    for (int i = 0; i < (tex->width * tex->height); i++, in++, out++) {
        uint32_t a = (*in >> 24) & 0xff;
        uint32_t b = (*in >> 16) & 0xff;
        uint32_t g = (*in >>  8) & 0xff;
        uint32_t r = (*in >>  0) & 0xff;
        *out = (a << 24) | (r << 16) | (g << 8) | b;
    }

    if (tex->swizzled) {
        swizzle_rect((uint8_t*)g_swizzle_buf,
                     tex->width, tex->height,
                     tex->addr,
                     tex->pitch,
                     4);
    }

    gfx_xbox_renderer_select_texture(g_last_tile_selected,
                                     g_tex_bindings[g_last_tile_selected]);
}

static void gfx_xbox_renderer_set_sampler_parameters(
    int tile,
    bool linear_filter,
    uint32_t cms,
    uint32_t cmt)
{
    assert(tile < 2);
    struct texture *tex = &g_tex_pool[g_tex_bindings[tile]];
    tex->linear_filter = linear_filter;
    tex->cms = cms;
    tex->cmt = cmt;
    gfx_xbox_renderer_select_texture(tile, g_tex_bindings[tile]);
}

static void gfx_xbox_renderer_set_depth_test(bool depth_test)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, depth_test);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_FUNC, 3); // LE
    pb_end(p);
}

static void gfx_xbox_renderer_set_depth_mask(bool z_upd)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_DEPTH_MASK, z_upd);
    pb_end(p);
}

static void gfx_xbox_renderer_set_zmode_decal(bool zmode_decal)
{
    float s = -2.0f;
    float b = -2.0f;
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_POLY_OFFSET_FILL_ENABLE, zmode_decal);
    if (zmode_decal) {
        p = pb_push1(p, NV097_SET_POLYGON_OFFSET_SCALE_FACTOR, *(uint32_t*)&s);
        p = pb_push1(p, NV097_SET_POLYGON_OFFSET_BIAS, *(uint32_t*)&b);
    }
    pb_end(p);
}

static void gfx_xbox_renderer_set_viewport(int x, int y, int width, int height)
{
    float m_composite[4][4];
    matrix_viewport(m_composite, x, -y, width, height);

    uint32_t *p = pb_begin();
    p = pb_push_transposed_matrix(p, NV097_SET_COMPOSITE_MATRIX,
                                     (float*)m_composite);
    p = pb_push4f(p, NV097_SET_VIEWPORT_OFFSET, 0, 0, 0, 0);
    p = pb_push2f(p, NV097_SET_CLIP_MIN, 0.0f, MAX_Z);
    pb_end(p);
}

static void gfx_xbox_renderer_set_scissor(int x, int y, int width, int height)
{
    // FIXME
}

static void gfx_xbox_renderer_set_use_alpha(bool use_alpha) {
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, use_alpha);
    if (use_alpha) {
        p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR,
                        NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA);
        p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR,
                        NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA);
    }
    pb_end(p);
}

static void gfx_xbox_renderer_init(void)
{
    pb_init();
    pb_show_front_screen();

    g_width = pb_back_buffer_width();
    g_height = pb_back_buffer_height();

    g_swizzle_buf = MmAllocateContiguousMemoryEx(
        SWIZZLE_BUF_SIDE_LEN*SWIZZLE_BUF_SIDE_LEN*4, 0, MAXRAM, 0, 0x404);
    assert(g_swizzle_buf != NULL);

#if SHOW_FPS
    g_start = g_now = g_last = GetTickCount();
    g_frames_total = g_frames = g_fps = 0;
#endif

    float m_identity[4][4];
    matrix_identity(m_identity);
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
                      NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_FIXED)
                 | MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
                        NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
    p = pb_push_transposed_matrix(p,
                                  NV097_SET_MODEL_VIEW_MATRIX,
                                  (float*)m_identity);
    p = pb_push_transposed_matrix(p,
                                  NV097_SET_INVERSE_MODEL_VIEW_MATRIX,
                                  (float*)m_identity);
    p = pb_push_transposed_matrix(p,
                                  NV097_SET_PROJECTION_MATRIX,
                                  (float*)m_identity);
    p = pb_push4(p, NV097_SET_VIEWPORT_OFFSET, 0, 0, 0, 0);
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 1);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
#if WIREFRAME
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                    NV097_SET_FRONT_POLYGON_MODE_V_LINE);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                    NV097_SET_FRONT_POLYGON_MODE_V_LINE);
#endif
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    for (int i = 0; i < 4; i++) {
        p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(i), 0x0003ffc0);
    }
    pb_end(p);

    gfx_xbox_renderer_set_viewport(0, 0, g_width, g_height);
}

static void gfx_xbox_renderer_on_resize(void)
{
}

static void gfx_xbox_renderer_start_frame(void)
{
    pb_reset();
    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, g_width, g_height);
    pb_fill(0, 0, g_width, g_height, 0x00000000);
    pb_erase_text_screen();

    uint32_t *p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for(int i = 0; i < 16; i++) {
        *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
    }
    pb_end(p);
}

static void gfx_xbox_renderer_draw_triangles(
    float buf_vbo[],
    size_t buf_vbo_len,
    size_t buf_vbo_num_tris)
{
    size_t size_of_vert = g_cur_shader->num_floats;
    size_t num_vertices = buf_vbo_num_tris * 3;
    assert(num_vertices <= 0xffff);
    assert(g_cur_shader != NULL);

    while(pb_busy()) {}

    // FIXME: Restructure loop to not have conditions inside
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES);
    for (unsigned int v = 0; v < num_vertices; v++) {
        float *vp = &buf_vbo[v*size_of_vert];

        // POSITION is specified first, but we stream it in last
        float position[4];
        position[0] = *vp++;
        position[1] = *vp++;
        position[2] = *vp++;
        position[3] = *vp++;

        // TEXTURE COORDS
        if (g_cur_shader->cc_features.used_textures[0]
            || g_cur_shader->cc_features.used_textures[1]) {
            int tex_num = g_tex_bindings[g_last_tile_selected];
            struct texture *tex = &g_tex_pool[tex_num];
            assert(tex->addr != NULL);
            float tu = *vp++;
            float tv = *vp++;
            // FIXME: TEX0/1 might need to be unnormalized if tex is npot for
            // whatever reason (are textures of different sizes ever mapped?)
            if (!tex->swizzled) {
                // Linear textures needs unnormalized coords
                tu *= tex->width;
                tv *= tex->height;
            }

            // Pass through single pair of texcoords to both texture0 and
            // texture1
            p = pb_push2f(p, NV097_SET_VERTEX_DATA2F_M
                    + NV2A_VERTEX_ATTR_TEXTURE0*2*sizeof(float), tu, tv);
            p = pb_push2f(p, NV097_SET_VERTEX_DATA2F_M
                    + NV2A_VERTEX_ATTR_TEXTURE1*2*sizeof(float), tu, tv);
        }

        // FOG
        if (g_cur_shader->cc_features.opt_fog) {
            assert(g_cur_shader->cc_features.num_inputs <= 1);
            if (g_cur_shader->do_fog) {
                int idx = g_cur_shader->cc_features.num_inputs;
                pb_push(p++, NV097_SET_VERTEX_DATA4F_M
                    + (NV2A_VERTEX_ATTR_DIFFUSE + idx)*4*sizeof(float), 4);
                *(float*)(p++) = *vp++;
                *(float*)(p++) = *vp++;
                *(float*)(p++) = *vp++;
                *(float*)(p++) = *vp++;
            } else {
                vp += 4;
            }
        }

        // INPUTS        
        // FIXME: shader def supports up to four from the verts. This case is
        // not handled
        assert(g_cur_shader->cc_features.num_inputs <= 2);
        for (int i = 0; i < g_cur_shader->cc_features.num_inputs; i++) {
            pb_push(p++, NV097_SET_VERTEX_DATA4F_M
                + (NV2A_VERTEX_ATTR_DIFFUSE + i)*4*sizeof(float), 4);
            *(float*)(p++) = *vp++;
            *(float*)(p++) = *vp++;
            *(float*)(p++) = *vp++;
            if (g_cur_shader->cc_features.opt_alpha) {
                *(float*)(p++) = *vp++;
            } else {
                *(float*)(p++) = 0.0f;
            }
        }

        // POSITION (last, to finish the vertex)
        pb_push(p++, NV097_SET_VERTEX4F, 4);
        *(float*)(p++) = position[0];
        *(float*)(p++) = position[1];
        *(float*)(p++) = position[2];
        *(float*)(p++) = position[3];
    }

    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}

static void gfx_xbox_renderer_end_frame(void)
{
}

static void gfx_xbox_renderer_finish_render(void)
{
        while(pb_busy());

#if SHOW_FPS
        pb_print("FPS: %d (%d)", g_fps, g_frames_total);
        pb_draw_text_screen();
#endif

        while(pb_busy());
        while (pb_finished());

#if SHOW_FPS
        g_frames++;
        g_frames_total++;
        g_now = GetTickCount();
        if ((g_now-g_last) > 1000) {
            g_fps = g_frames;
            g_frames = 0;
            g_last = g_now;
        }
#endif
}

struct GfxWindowManagerAPI gfx_xbox_wm_api = {
    gfx_xbox_wm_init,
    gfx_xbox_wm_set_keyboard_callbacks,
    gfx_xbox_wm_set_fullscreen_changed_callback,
    gfx_xbox_wm_set_fullscreen,
    gfx_xbox_wm_main_loop,
    gfx_xbox_wm_get_dimensions,
    gfx_xbox_wm_handle_events,
    gfx_xbox_wm_start_frame,
    gfx_xbox_wm_swap_buffers_begin,
    gfx_xbox_wm_swap_buffers_end,
    gfx_xbox_wm_get_time
};

struct GfxRenderingAPI gfx_xbox_renderer_api = {
    gfx_xbox_renderer_z_is_from_0_to_1,
    gfx_xbox_renderer_unload_shader,
    gfx_xbox_renderer_load_shader,
    gfx_xbox_renderer_create_and_load_new_shader,
    gfx_xbox_renderer_lookup_shader,
    gfx_xbox_renderer_shader_get_info,
    gfx_xbox_renderer_new_texture,
    gfx_xbox_renderer_select_texture,
    gfx_xbox_renderer_upload_texture,
    gfx_xbox_renderer_set_sampler_parameters,
    gfx_xbox_renderer_set_depth_test,
    gfx_xbox_renderer_set_depth_mask,
    gfx_xbox_renderer_set_zmode_decal,
    gfx_xbox_renderer_set_viewport,
    gfx_xbox_renderer_set_scissor,
    gfx_xbox_renderer_set_use_alpha,
    gfx_xbox_renderer_draw_triangles,
    gfx_xbox_renderer_init,
    gfx_xbox_renderer_on_resize,
    gfx_xbox_renderer_start_frame,
    gfx_xbox_renderer_end_frame,
    gfx_xbox_renderer_finish_render
};
#endif
