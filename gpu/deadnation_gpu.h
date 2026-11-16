/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef DEADNATION_GPU_H
#define DEADNATION_GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    const char *splash;         /* start-up picture (the dump's sce_sys/pic1.png), NULL: none */
} DeadNationGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void deadnation_gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int deadnation_gpu_init(const DeadNationGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t deadnation_gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int deadnation_gpu_handle_fault(void *ucontext, void *address);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int deadnation_gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int deadnation_gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int deadnation_gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int deadnation_gpu_choice_poll(void);
/* Confirmation with a separate preview of the new save; PNG data is decoded before returning. */
int deadnation_gpu_choice_confirm(const char *title, const char *const *items, int count, int focus,
                                 const unsigned char *png, uint64_t size, const char *caption, const char *unavailable);
/* Scripted runs (DEADNATION_PAD_FILE "settings[=row]"): opens the Display Settings panel on `row`. */
void deadnation_gpu_open_display_settings(int row);
/* 1 while a dialog (save list, text entry) is shown: the game's pad input is held neutral. */
int deadnation_gpu_overlay_captures_input(void);
/* Opens the Display Settings panel (F10, or Select held for 2 s: runtime_pad.c); 1 while it is open. */
void deadnation_display_settings_open(void);
int deadnation_display_settings_is_open(void);
/* Asks "Exit DeadNation?" (No focused); Yes closes the game as closing its window does. Any thread. */
void deadnation_exit_prompt_open(void);
/* settings.ini title_back_exits (DEADNATION_TITLE_BACK_EXITS, default on): Back on the title screen
 * opens the exit prompt (runtime_pad.c). Any thread. */
int deadnation_title_back_exits(void);
/* Ends the game as closing its window does (any thread). */
void deadnation_gpu_request_quit(void);
/* settings.ini render_resolution (DEADNATION_RENDER_RESOLUTION): the lines the game renders at (720,
 * 1080, 1440 or 2160) and the width for the output's shape at those lines, or, narrower than 16:9, the
 * 16:9 width and the shape's height (1920x1440 at 4:3 and 1080p); the video out and the render
 * device (runtime_game.c) take that size. deadnation_render_4k is 1 above 1080p (PS4 Pro mode and
 * its larger memory pool). Valid once deadnation_gpu_init returned. */
int deadnation_render_4k(void);
int deadnation_render_height(void);
int deadnation_render_width(void);
/* The game's display width at 2160 lines: 3840 (16:9) for a 16:9 or narrower output, up to 7680
 * (32:9) for a wider one; the output at start decides (the window's size, the desktop's in
 * borderless). The video out is that wide (half with render_resolution=1080p) and
 * runtime_game_widen widens the engine to match. Valid once deadnation_gpu_init returned. */
int deadnation_width_4k(void);
/* Sets it back (3840) when the engine could not be widened; before the game opens its video out. */
void deadnation_set_width_4k(int width);
/* 0: DeadNation has no HDR output (it always configures SDR), so the presenter never uses an HDR
 * surface. */
int deadnation_hdr(void);
/* 1 when settings.ini skip_intro (DEADNATION_SKIP_INTRO, default on) is on: the game starts without
 * its start-up logos and notices (-nosplash). Valid once deadnation_gpu_init returned. */
int deadnation_skip_intro(void);
/* settings.ini frame_rate (DEADNATION_FRAME_RATE): 30, 60, 90, 120, 144 or 180; the Display Settings panel
 * changes it while the game runs. The PS4 vblank runs at it and the engine's target frame rate is set to
 * match at each flip (runtime_game.c). Valid once deadnation_gpu_init returned; any thread. */
int deadnation_frame_rate(void);
/* settings.ini debug_speed (DEADNATION_DEBUG_SPEED, debugging): the game runs 1 to 4 times faster
 * than real time (runtime_game.c update_count); 1 by default. Any thread. */
int deadnation_debug_speed(void);
/* Sets frame_rate (one of 30, 60, 90, 120, 144, 180) until the next start; any thread. */
void deadnation_set_frame_rate(int rate);
/* sceCommonDialogInitialize ran (runtime_services.c): the upstream dialog libraries check it. */
void deadnation_gpu_common_dialog_initialized(void);
/* Speaker layout for the game's 7.1 audio (settings.ini audio_channels): 0 automatic (the
 * device's), 1 stereo, 2 5.1, 3 7.1. Read when an audio port is opened. */
int deadnation_gpu_audio_channels(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned deadnation_gpu_symbol_count(void);
/* Yields of threads waiting for guest memory copies (CPU report). */
unsigned long long deadnation_gpu_copy_yields(void);
/* The hang dump's GPU lines: how far the GPU got on each timeline it is waited on. */
void deadnation_gpu_hang_report(void);
/* Frames handed to the presenter since start (hang dump). */
unsigned long long deadnation_gpu_flips(void);
/* A notice in the corner for `milliseconds` (any thread; the runtime's messages to the player). */
void deadnation_gpu_notice(const char *text, int milliseconds);
/* Trophy unlocked (any thread): its name as a notice in the corner. The other arguments are kept
 * for the runtime's call: id, description and PNG icon. */
void deadnation_gpu_toast_trophy(int id, const char *title, const char *detail, const unsigned char *png,
                                 uint64_t png_size);
#ifdef __cplusplus
}
#endif
#endif
