/* Dead Nation specifics. The game's version is logged first so a bug report's log tells which dump
 * it came from: param.sfo's TITLE_ID and APP_VER, and the eboot's SHA-256 that scripts/prepare.py
 * records in analysis.json beside the image. The build picks the addresses of the screen
 * manager's methods, which the runner hooks to know which menu screen is showing (skip_intro), and
 * of the engine's frame clock (frame_rate). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "gpu/deadnation_gpu.h"

/* eboot.bin builds the runner has been run with, and in each: the screen manager's two methods
 * that change its stack of screens (push(manager, screen) and pop-to(manager, ...)), the global
 * holding the frame clock, the end of the code and the data segment holding the clock's vtable.
 * The setup warns about other builds: keep kTested* in setup/setup.cpp in step. */
typedef struct {
    const char *sha256, *version;
    uint32_t screen_push, screen_pop, clock, code_end, data, data_end, render_size, persp_call, ortho_call, finalize, view_rect, ortho_aspect, view_fov;
    uint32_t item_call, item_runner; /* the items' script runner and its call (patch_logic) */
    uint32_t update_stop, update_loop; /* the update loop's stop test and loop test (patch_updates) */
    /* Cheats (0: none for this build): the session global and its vtable, the damage function's
     * god-mode test, the weapon table, the upgrade multiplier, the debug menu's armor unlock. */
    uint32_t session, session_vtable, score_getter, god_test, weapon_table, upgrade_scale;
    /* unlock_armor: the armor sets' global, vtable and array getter; the profile's global, vtable
     * and unlock list getters (campaign, Road of Devastation) */
    uint32_t armor_sets, armor_sets_vtable, armor_sets_getter, profile, profile_vtable, campaign_lists, road_lists;
    uint32_t timer_start, rush_tag; /* a script timer's start (patch_dash) and the dash timer's tag */
} Build;
static const Build known_builds[]={
    {"09b367a222cfb6308811650ae1fb8105ea6c7726137646907b211419027aa37c","01.03",
     0xe21940,0xe21150,0x1bf6958,0x13b4920,0x13b8000,0x1bf6ef0,0xc88010,0xce352a,0xce3658,0xce2cf0,0xcebbd1,0xceb3db,0xddadd7,
     0xdf04d8,0xdf38f0,0xcafa65,0xcafb00,
     0x1bf6828,0x13c7c90,0x4749f0,0x494ad9,0xf536f0,0x49cd20,
     0x1bf6898,0x13ce1d0,0x4d4d50,0x1bf67f8,0x13c53b0,0x449250,0x4492d0,0x490b27,0x18b1098},
    {"8d2ae241ca42585d0b2601554720f198a7ef63c0b6f80fc34325883530ab8135","01.00",
     0xe1f600,0xe1f4b0,0x1bf26f8,0x13b2418,0x13b4000,0x1bf2c90,0xc866d0,0xce17aa,0xce18d8,0xce0f70,0xce9e51,0xce965b,0xdd9837,
     0xdeeca8,0xdf20c0,0xcadce5,0xcadd80,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, /* cheats: 01.03 only (the setup requires the update) */
};
static const Build *build;
static unsigned char *game_image;

/* "source_sha256" from analysis.json in the folder of `image_path`. */
static int eboot_sha256(const char *image_path, char *out) {
    char path[4096];
    const char *slash=strrchr(image_path,'/'), *back=strrchr(image_path,'\\');
    if (back>slash) slash=back;
    const int dir=slash ? (int)(slash-image_path)+1 : 0;
    snprintf(path,sizeof(path),"%.*sanalysis.json",dir,image_path);
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    char head[4096];
    const size_t n=fread(head,1,sizeof(head)-1,f);
    fclose(f);
    head[n]=0;
    const char *sha=strstr(head,"\"source_sha256\": \"");
    if (!sha || strspn(sha+18,"0123456789abcdef")<64) return 0;
    memcpy(out,sha+18,64); out[64]=0;
    return 1;
}

void runtime_game_identify(const char *title_id, const char *app_version, const char *app0, const char *image_path) {
    (void)app0;
    char sha[65]="";
    if (eboot_sha256(image_path,sha))
        for (size_t i=0;i<sizeof(known_builds)/sizeof(*known_builds);++i)
            if (!strcmp(sha,known_builds[i].sha256)) build=&known_builds[i];
    printf("Game: %s version %s, eboot.bin sha256 %s (%s%s)\n",title_id[0] ? title_id : "?",
           app_version[0] ? app_version : "unknown",sha[0] ? sha : "unknown",
           build ? "known build " : "a build the runner was not checked with",build ? build->version : "");
}

/* The screen manager (Screen_Bootstrap, Screen_Main_Menu, Screen_Game, ...) keeps its screens in
 * a vector of refcounted pointers at +0xa8 (begin) / +0xb0 (end); the last is the one showing.
 * Each screen's vtable slot 2 returns its name. The manager is an object the game passes around,
 * so the push and pop methods are hooked at their entry: a stub records the manager (rdi) in
 * `screen_manager`, replays the 6 bytes of prologue it replaced (push rbp; mov rbp,rsp;
 * push r15) and jumps back.
 * It also ends the frame's updates (patch_updates) after the one running: the PS4 ran one update a
 * frame, so a screen change was always followed by a render before the next update. A second update
 * in the same frame after the mission end (Screen_Post_Game_Save) crashed the game in the libc free
 * of a render resource, released twice (debug_speed=2, or 4K at 144 FPS short of its rate). */
static void *volatile screen_manager;
static const unsigned char screen_prologue[6]={0x55,0x48,0x89,0xe5,0x41,0x57};
/* patch_updates: the updates the next frame runs (`wanted`, set at each flip), those it has run
 * (`done`, written by the loop after each update). */
static struct { volatile int32_t wanted, done; } updates={1,0};

static unsigned char *hook_entry(unsigned char *image, uint32_t at, unsigned char *stub) {
    unsigned char *t=stub;
    const uint64_t slot=(uint64_t)(uintptr_t)&screen_manager, wanted=(uint64_t)(uintptr_t)&updates.wanted;
    *t++=0x50;                                                  /* push rax */
    *t++=0x48; *t++=0xb8; memcpy(t,&slot,8); t+=8;              /* mov rax,&screen_manager */
    *t++=0x48; *t++=0x89; *t++=0x38;                            /* mov [rax],rdi */
    *t++=0x48; *t++=0xb8; memcpy(t,&wanted,8); t+=8;            /* mov rax,&updates.wanted */
    *t++=0xc7; *t++=0x00; memset(t,0,4); t+=4;                  /* mov dword [rax],0 */
    *t++=0x58;                                                  /* pop rax */
    memcpy(t,screen_prologue,6); t+=6;                          /* the replaced prologue */
    const int32_t back=(int32_t)((image+at+6)-(t+5));
    *t++=0xe9; memcpy(t,&back,4); t+=4;                         /* jmp entry+6 */
    const int32_t to=(int32_t)(stub-(image+at+5));
    image[at]=0xe9; memcpy(image+at+1,&to,4); image[at+5]=0x90; /* jmp stub; nop */
    return t;
}

/* settings.ini render_resolution and aspect_ratio. The render device (01.03 constructor 0xc87fd0,
 * global +0x1bf6908) takes one size from its init parameters, 1920x1080, and stores it at +8; every
 * render target, the display buffers included, is made that size. The constructor copies it with
 * mov rax,[r14]; mov [r15+8],rax (01.03 0xc88010); a jmp there to a stub that stores the wanted
 * size instead renders the game at 2560x1440 or 3840x2160, and wider than 16:9 for 21:9 and 32:9
 * (deadnation_render_width: the same lines, wider). */
static const unsigned char size_site[7]={0x49,0x8b,0x06,0x49,0x89,0x47,0x08};
static int render_size_found(unsigned char *image) {
    return build && !memcmp(image+build->render_size,size_site,sizeof(size_site));
}
static void patch_render_size(unsigned char *image, unsigned char *stub) {
    const uint32_t w=(uint32_t)deadnation_render_width(), h=(uint32_t)deadnation_render_height();
    if (w==1920 && h==1080) return;
    unsigned char *at=image+build->render_size;
    if (!render_size_found(image)) {
        printf("Render: the render device is not where eboot %s has it; the game renders at 1920x1080\n",build->version);
        return;
    }
    const uint64_t size=w|((uint64_t)h<<32);
    unsigned char *t=stub;
    *t++=0x48; *t++=0xb8; memcpy(t,&size,8); t+=8;                 /* mov rax,size */
    memcpy(t,size_site+3,4); t+=4;                                  /* mov [r15+8],rax */
    const int32_t back=(int32_t)((at+7)-(t+5));
    *t++=0xe9; memcpy(t,&back,4);                                   /* jmp site+7 */
    const int32_t to=(int32_t)(stub-(at+5));
    at[0]=0xe9; memcpy(at+1,&to,4); at[5]=at[6]=0x90;
    printf("Render: the game renders at %ux%u\n",(unsigned)w,(unsigned)h);
}

/* 4:3 and 16:10: the render size is taller than 16:9 (1920x1440 at 4:3). The game would keep
 * its vertical view and the menus' height and cut the sides. Every camera (the level's
 * perspective; the menus' and HUD's ortho) is set up by one method (01.03 0xceb3xx) from its
 * target's size at [rbp-0x48] and shape at [rbp-0x40] (stored at 0xceb3db): the perspective
 * (0xce33f0) or ortho (0xce3560, when [r12] is set) builder makes the projection matrix and hands
 * it (rdx) to 0xce2cf0, which stores it with the view matrix and all derived from both. For the
 * render size, the ortho cameras get 16:9 for the shape (x in -16/9..16/9 fills the width, the
 * menus' 16:9 layout), and both builders' calls go to a stub that scales the matrix's y term by
 * (w/h)/(16/9): 16:9's width, and more of the level above and below, with the menus and HUD
 * whole and centred. The stub finds the size in its caller's frame, [[rbp]-0x48]. */
static const unsigned char aspect_site[5]={0xc5,0xfa,0x11,0x45,0xc0}; /* vmovss [rbp-0x40],xmm0 */
static const unsigned char view_rect_site[6]={0xc5,0xf9,0x7e,0xc0,0x89,0xc1}; /* vmovd eax,xmm0; mov ecx,eax */
static const unsigned char fov_site[8]={0xc5,0xfa,0x10,0x85,0x74,0xff,0xff,0xff}; /* vmovss xmm0,[rbp-0x8c] */
static int cameras_found(const unsigned char *image) {
    if (!build || !build->ortho_aspect || memcmp(image+build->ortho_aspect,aspect_site,sizeof(aspect_site)) ||
        memcmp(image+build->view_fov,fov_site,sizeof(fov_site)) ||
        memcmp(image+build->view_rect,view_rect_site,sizeof(view_rect_site))) return 0;
    const uint32_t sites[2]={build->persp_call,build->ortho_call};
    for (int i=0;i<2;++i) {
        int32_t rel;
        memcpy(&rel,image+sites[i]+1,4);
        if (image[sites[i]]!=0xe8 || sites[i]+5+rel!=build->finalize) return 0;
    }
    return 1;
}
/* The camera also keeps its field of view and shape (0xce2cf0 stores xmm0..xmm3, diagonal field
 * of view, near, far, aspect, at +0x100), and what the level draws is culled by a frustum rebuilt
 * from them: tan(y/2) = tan(fov/2)/sqrt(1+aspect), tan(x/2) = aspect*tan(y/2). For the render
 * size this rewrites fov and aspect for the taller picture: same x, y by camera_taller. p: fov,
 * aspect; frame: the camera method's frame (its target's size at -0x48). */
static float camera_taller;
static uint64_t camera_size;
static void ABI camera_view(float *p, const unsigned char *frame) {
    uint64_t size;
    memcpy(&size,frame-0x48,8);
    if (size!=camera_size || !(p[0]>0.0f && p[0]<3.1f) || !(p[1]>0.0f)) return;
    const float t=tanf(p[0]*0.5f), y=t/sqrtf(1.0f+p[1])*camera_taller, aspect=p[1]/camera_taller;
    p[0]=2.0f*atanf(y*sqrtf(1.0f+aspect));
    p[1]=aspect;
}
static unsigned char *patch_camera(unsigned char *image, unsigned char *stub) {
    const uint32_t w=(uint32_t)deadnation_render_width(), h=(uint32_t)deadnation_render_height();
    if (w*9>=h*16) return stub;
    const uint32_t sites[2]={build->persp_call,build->ortho_call};
    const float scale=(float)w*9.0f/((float)h*16.0f), wide=16.0f/9.0f;
    const uint64_t size=w|((uint64_t)h<<32);
    unsigned char *t=stub;
    *t++=0x51;                                                      /* push rcx */
    *t++=0x48; *t++=0x8b; *t++=0x4d; *t++=0x00;                     /* mov rcx,[rbp] (the caller's frame) */
    *t++=0x50;                                                      /* push rax */
    *t++=0x48; *t++=0xb8; memcpy(t,&size,8); t+=8;                 /* mov rax,size */
    *t++=0x48; *t++=0x39; *t++=0x41; *t++=0xb8;                     /* cmp [rcx-0x48],rax */
    *t++=0x58;                                                      /* pop rax */
    *t++=0x75; *t++=21;                                             /* jne skip */
    *t++=0xf3; *t++=0x44; *t++=0x0f; *t++=0x10; *t++=0x7a; *t++=0x14; /* movss xmm15,[rdx+0x14] */
    *t++=0xf3; *t++=0x44; *t++=0x0f; *t++=0x59; *t++=0x3d; unsigned char *k=t; t+=4; /* mulss xmm15,[scale] */
    *t++=0xf3; *t++=0x44; *t++=0x0f; *t++=0x11; *t++=0x7a; *t++=0x14; /* movss [rdx+0x14],xmm15 */
    *t++=0x59;                                                      /* skip: pop rcx */
    const int32_t to_finalize=(int32_t)((image+build->finalize)-(t+5));
    *t++=0xe9; memcpy(t,&to_finalize,4); t+=4;                      /* jmp finalize */
    int32_t d=(int32_t)(t-(k+4)); memcpy(k,&d,4);
    memcpy(t,&scale,4); t+=4;
    /* The perspective call first has camera_view rewrite xmm0 (fov) and xmm3 (aspect). */
    camera_taller=1.0f/scale; camera_size=size;
    unsigned char *view=t;
    static const unsigned char save[]={0x57,0x56,0x52,0x48,0x83,0xec,0x20, /* push rdi,rsi,rdx; sub rsp,32 */
        0xf3,0x0f,0x11,0x04,0x24, 0xf3,0x0f,0x11,0x5c,0x24,0x04,           /* [rsp]=xmm0, [rsp+4]=xmm3 */
        0xf3,0x0f,0x11,0x4c,0x24,0x08, 0xf3,0x0f,0x11,0x54,0x24,0x0c,      /* [rsp+8]=xmm1, [rsp+12]=xmm2 */
        0x48,0x89,0xe7, 0x48,0x8b,0x75,0x00};                              /* mov rdi,rsp; mov rsi,[rbp] */
    memcpy(t,save,sizeof(save)); t+=sizeof(save);
    const uint64_t fn=(uint64_t)(uintptr_t)camera_view;
    *t++=0x48; *t++=0xb8; memcpy(t,&fn,8); t+=8;                   /* mov rax,camera_view */
    *t++=0xff; *t++=0xd0;                                           /* call rax */
    static const unsigned char restore[]={0xf3,0x0f,0x10,0x04,0x24, 0xf3,0x0f,0x10,0x5c,0x24,0x04,
        0xf3,0x0f,0x10,0x4c,0x24,0x08, 0xf3,0x0f,0x10,0x54,0x24,0x0c,
        0x48,0x83,0xc4,0x20,0x5a,0x5e,0x5f};                               /* add rsp,32; pop rdx,rsi,rdi */
    memcpy(t,restore,sizeof(restore)); t+=sizeof(restore);
    const int32_t to_common=(int32_t)(stub-(t+5));
    *t++=0xe9; memcpy(t,&to_common,4); t+=4;                        /* jmp the y-scaling stub */
    for (int i=0;i<2;++i) {
        unsigned char *target=i ? stub : view;
        const int32_t to=(int32_t)(target-(image+sites[i]+5));
        memcpy(image+sites[i]+1,&to,4);                             /* call stub */
    }
    unsigned char *at=image+build->ortho_aspect, *start=t;
    *t++=0x41; *t++=0x83; *t++=0x3c; *t++=0x24; *t++=0x00;          /* cmp dword [r12],0 (0: perspective) */
    *t++=0x74; *t++=26;                                             /* je skip */
    *t++=0x51;                                                      /* push rcx */
    *t++=0x48; *t++=0xb9; memcpy(t,&size,8); t+=8;                 /* mov rcx,size */
    *t++=0x48; *t++=0x39; *t++=0x4d; *t++=0xb8;                     /* cmp [rbp-0x48],rcx */
    *t++=0x59;                                                      /* pop rcx */
    *t++=0x75; *t++=8;                                              /* jne skip */
    *t++=0xf3; *t++=0x0f; *t++=0x10; *t++=0x05; k=t; t+=4;          /* movss xmm0,[wide] */
    memcpy(t,aspect_site,sizeof(aspect_site)); t+=sizeof(aspect_site); /* skip: the replaced store */
    const int32_t back=(int32_t)((at+5)-(t+5));
    *t++=0xe9; memcpy(t,&back,4); t+=4;                             /* jmp site+5 */
    d=(int32_t)(t-(k+4)); memcpy(k,&d,4);
    memcpy(t,&wide,4); t+=4;
    const int32_t to=(int32_t)(start-(at+5));
    at[0]=0xe9; memcpy(at+1,&to,4);
    /* What the level draws is culled by the view the renderer builds each frame (01.03 0xddad30:
     * view matrix, then horizontal and vertical field of view at +0x40, near, far), which takes
     * its shape from the render device's 16:9 and the camera's field of view: objects above and
     * below 16:9 would only appear as they cross its edge (pop-in at the top and bottom). Tan of
     * half the vertical field is at [rbp-0x8c] when 0xddadd7 loads it for the atan; it is scaled
     * like the projection's y term. */
    at=image+build->view_fov; start=t;
    const float taller=1.0f/scale;
    memcpy(t,fov_site,sizeof(fov_site)); t+=sizeof(fov_site);       /* the replaced load */
    *t++=0xf3; *t++=0x0f; *t++=0x59; *t++=0x05; k=t; t+=4;          /* mulss xmm0,[taller] */
    const int32_t back_fov=(int32_t)((at+sizeof(fov_site))-(t+5));
    *t++=0xe9; memcpy(t,&back_fov,4); t+=4;                         /* jmp site+8 */
    d=(int32_t)(t-(k+4)); memcpy(k,&d,4);
    memcpy(t,&taller,4); t+=4;
    const int32_t to_fov=(int32_t)(start-(at+5));
    at[0]=0xe9; memcpy(at+1,&to_fov,4); at[5]=at[6]=at[7]=0x90;
    printf("Render: the cameras keep 16:9's width and show %.0f%% more height\n",(double)(100.0f/scale-100.0f));
    return t;
}

/* The menus and HUD lay out in their camera's view rect, (-a,-1,a,1) for a target of shape a
 * (01.03 0xcebbbf: the target's pixel aspect times w/h, then vmovd eax,xmm0; mov ecx,eax at
 * 0xcebbd1 stores it). For the render size, a stub gives it 16:9 instead, so they keep the 16:9
 * layout that patch_camera fits to the width. */
static unsigned char *patch_view_rect(unsigned char *image, unsigned char *stub) {
    const uint32_t w=(uint32_t)deadnation_render_width(), h=(uint32_t)deadnation_render_height();
    if (w*9>=h*16) return stub;
    unsigned char *at=image+build->view_rect;
    const uint64_t size=w|((uint64_t)h<<32);
    const float wide=16.0f/9.0f;
    unsigned char *t=stub;
    *t++=0x51;                                                      /* push rcx */
    *t++=0x48; *t++=0xb9; memcpy(t,&size,8); t+=8;                 /* mov rcx,size */
    *t++=0x48; *t++=0x39; *t++=0x4d; *t++=0xd8;                     /* cmp [rbp-0x28],rcx (the target's size) */
    *t++=0x59;                                                      /* pop rcx */
    *t++=0x75; *t++=8;                                              /* jne skip */
    *t++=0xf3; *t++=0x0f; *t++=0x10; *t++=0x05; unsigned char *k=t; t+=4; /* movss xmm0,[wide] */
    memcpy(t,view_rect_site,sizeof(view_rect_site)); t+=sizeof(view_rect_site); /* skip: the replaced code */
    const int32_t back=(int32_t)((at+6)-(t+5));
    *t++=0xe9; memcpy(t,&back,4); t+=4;                             /* jmp site+6 */
    const int32_t d=(int32_t)(t-(k+4)); memcpy(k,&d,4);
    memcpy(t,&wide,4); t+=4;
    const int32_t to=(int32_t)(stub-(at+5));
    at[0]=0xe9; memcpy(at+1,&to,4); at[5]=0x90;
    return t;
}

/* Weapons and items run Lua scripts that count update ticks, not seconds: the SMG adds 1 to a
 * counter each tick and fires at 4, the rifle waits 16 ticks between shots. The game ran them at
 * its 30 FPS, one tick a frame, so at 120 FPS weapons fired four times as fast. They keep 30
 * ticks a second at any frame rate. The items' job runs every frame (it also moves state between
 * the world's double buffers and builds what is drawn); per item it calls a runner that
 * evaluates the item's script (01.03 0xdf38f0 at 0xdf04d8, 01.00 0xdf20c0 at 0xdeeca8) with the
 * frame's time in xmm0 and at [rdx], the item's context. A stub asks `item_events` first: on
 * updates without a tick the call is skipped, and on a tick the runner gets 1/30 s. Which of the
 * next frame's updates tick (`tick_mask`, by `updates.done`; `logic_tick` when the update loop
 * is not patched) is set at each flip (update_count). The characters' and directors' runners are
 * left alone.
 * Events last one frame, so a tick sees those of the frames since the last one. A script's event
 * test (01.03 0xdf0190) looks in two places: the events sent to the item, gathered into its
 * context each frame (0xdf3400, after the job zeroes their count at 0xdeb356: an "action" from the
 * player's R2), then the frame's area events, those raised within reach of the item (a switch's
 * "door-open" to its gate). Some are sent once (an "action"), some every frame while a button is
 * down (a weapon's "fire", which shoots on a new press), so a tick sees an event that came on any
 * frame since the last tick, except one the last tick saw that was missing on a frame since: that
 * tick sees it released and the next one sees it again if it came back (a turbo button's presses
 * and releases, at most 15 of each a second). At 30 FPS every frame is a tick and nothing changes. */
static volatile unsigned char logic_tick=1;
/* Which of the frame's updates (patch_updates) tick the item scripts. */
static volatile uint32_t tick_mask=1;
static int updates_patched;
enum { item_slots=8192, item_event_max=19, item_tracked=24 };
typedef struct {
    uint32_t hash; float value, carried;
    uint8_t here, seen, gap, again, last, carry; /* this frame; since the last tick: came, was missing,
                                                     came after missing; the last tick had it; the next
                                                     tick gets it (came back after a release) */
} ItemTrack;
typedef struct { int32_t script, count; ItemTrack events[item_tracked]; } ItemTracks;
static ItemTracks item_tracks[item_slots];

/* The tracked event `hash` of an item, added when missing (NULL when the list is full). */
static ItemTrack *track_event(ItemTracks *t, uint32_t hash) {
    for (int32_t k=0;k<t->count;++k) if (t->events[k].hash==hash) return &t->events[k];
    if (t->count==item_tracked) return NULL;
    ItemTrack *e=&t->events[t->count++];
    memset(e,0,sizeof(*e));
    e->hash=hash;
    return e;
}

/* Before an item's runner, on the item job's thread: 1 to run it. The context holds the frame's
 * time at +0, the item's slot at +0xc, its record at +0x18 (script id at +0x34, position at
 * +0x250), the area events at +0x60 (32 bytes each: hash +4, value +8, reach +0xc, position
 * +0x10) and their count at +0x68, the count of events sent to it at +0x8c and those events
 * (hash, value) at +0x94. On a tick the events sent are rewritten to those the tick sees (the
 * values of an event's frames add up, as the gather adds an event sent twice); area events the
 * tick frame has stay where the test finds them. */
static int ABI item_events(unsigned char *ctx) {
    const int32_t slot=*(int32_t *)(ctx+0xc);
    const unsigned char *record=*(unsigned char **)(ctx+0x18);
    int32_t *count=(int32_t *)(ctx+0x8c);
    if (slot<0 || slot>=item_slots || *count<0 || *count>item_event_max) return 1; /* runs each frame */
    const int32_t script=*(int32_t *)(record+0x34);
    uint32_t *events=(uint32_t *)(ctx+0x94); /* hash, value (float bits) */
    ItemTracks *t=&item_tracks[slot];
    if (t->script!=script) { t->script=script; t->count=0; }
    for (int32_t k=0;k<t->count;++k) t->events[k].here=0;
    for (int32_t i=0;i<*count;++i) {
        ItemTrack *e=track_event(t,events[2*i]);
        if (e) { float v; memcpy(&v,&events[2*i+1],4); e->value+=v; e->here=1; }
    }
    const int32_t areas=*(int32_t *)(ctx+0x68);
    const unsigned char *area=*(unsigned char **)(ctx+0x60);
    const float *at=(const float *)(record+0x250);
    for (int32_t i=0;i<areas && area;++i,area+=32) {
        const float *p=(const float *)(area+0x10), dx=at[0]-p[0], dy=at[1]-p[1], dz=at[2]-p[2];
        float reach, value; uint32_t hash;
        memcpy(&hash,area+4,4); memcpy(&value,area+8,4); memcpy(&reach,area+0xc,4);
        if (reach<sqrtf(dx*dx+dy*dy+dz*dz)) continue;
        ItemTrack *e=track_event(t,hash);
        if (e && !e->here) { if (!e->seen) e->value=value; e->here=1; }
    }
    for (int32_t k=0;k<t->count;++k) {
        ItemTrack *e=&t->events[k];
        if (e->here) { if (e->gap && e->last) e->again=1; e->seen=1; }
        else e->gap=1;
    }
    const int32_t update=updates_patched ? updates.done : 0;
    if (!(updates_patched ? update<32 && (tick_mask>>update&1) : logic_tick)) return 0;
    int32_t n=0, kept=0;
    for (int32_t k=0;k<t->count;++k) {
        ItemTrack e=t->events[k];
        const int released=e.last && e.gap && !e.carry;
        const int sees=e.carry || (e.seen && !released);
        if (sees && n<item_event_max) {
            const float v=e.carry ? e.carried : e.value;
            events[2*n]=e.hash; memcpy(&events[2*n+1],&v,4); ++n;
        }
        e.carry=released && e.again; e.carried=e.value; e.last=(uint8_t)sees;
        e.value=0; e.seen=e.gap=e.again=0;
        if (e.last || e.carry) t->events[kept++]=e;
    }
    t->count=kept;
    *count=n;
    return 1;
}

static unsigned char *patch_logic(unsigned char *image, unsigned char *stub) {
    unsigned char *at=image+build->item_call;
    int32_t rel; memcpy(&rel,at+1,4);
    if (at[0]!=0xe8 || build->item_call+5+rel!=build->item_runner) {
        printf("Scripts: the items' script runner is not where eboot %s has it; weapons run once a frame\n",build->version);
        return stub;
    }
    const float tick_dt=1.0f/30;
    const uint64_t fn=(uint64_t)(uintptr_t)item_events;
    unsigned char *t=stub;
    static const unsigned char save[]={0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51, /* push rdi,rsi,rdx,rcx,r8,r9 */
        0x48,0x83,0xec,0x08, 0x48,0x89,0xd7};                                  /* sub rsp,8; mov rdi,rdx */
    memcpy(t,save,sizeof(save)); t+=sizeof(save);
    *t++=0x48; *t++=0xb8; memcpy(t,&fn,8); t+=8;            /* mov rax,item_events */
    *t++=0xff; *t++=0xd0;                                   /* call rax */
    static const unsigned char restore[]={0x48,0x83,0xc4,0x08, 0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f, /* add rsp,8; pop */
        0x85,0xc0, 0x74,31};                                                    /* test eax,eax; je skip */
    memcpy(t,restore,sizeof(restore)); t+=sizeof(restore);
    *t++=0x52;                                              /* push rdx */
    *t++=0xff; *t++=0x32;                                   /* push qword [rdx]: the frame's time */
    *t++=0x48; *t++=0x83; *t++=0xec; *t++=0x08;             /* sub rsp,8 */
    *t++=0xc7; *t++=0x02; memcpy(t,&tick_dt,4); t+=4;       /* mov dword [rdx],1/30 */
    *t++=0xc5; *t++=0xfa; *t++=0x10; *t++=0x02;             /* vmovss xmm0,[rdx] */
    rel=(int32_t)((image+build->item_runner)-(t+5));
    *t++=0xe8; memcpy(t,&rel,4); t+=4;                      /* call runner */
    *t++=0x48; *t++=0x83; *t++=0xc4; *t++=0x08;             /* add rsp,8 */
    *t++=0x59; *t++=0x5a;                                   /* pop rcx; pop rdx */
    *t++=0x89; *t++=0x0a;                                   /* mov [rdx],ecx */
    *t++=0xc3;                                              /* ret */
    *t++=0xc3;                                              /* skip: ret */
    const int32_t to=(int32_t)(stub-(at+5));
    at[0]=0xe8; memcpy(at+1,&to,4);                         /* call stub */
    return t;
}

/* The game's update loop (01.03 0xcaf980, 01.00 0xcadc00) runs one update a frame of the clock's
 * step (1/fps) and drops the rest of the time the frame took: its test after an update stops when
 * one update has run (or the update took longer than a step), so a frame slower than 1/fps slowed
 * the game down (240 FPS shown at 180 ran at 3/4 speed). Patched, the loop runs `updates.wanted`
 * updates of 1/fps, set at each flip from the real time since the last one (update_count), and
 * writes the number run to `updates.done`. A fixed step keeps every system as it is at the frame
 * rate (a step of the frame's time instead broke the zombies' walk). The stop test becomes
 * inc ebx; mov rax,&updates; mov [rax+4],ebx; cmp ebx,[rax]; setge al; or al,r13b; nop (the loop
 * then sets r13b from al), and the loop test (remaining time >= step) becomes "until r13b". */
static void patch_updates(unsigned char *image) {
    static const unsigned char stop[24]={0xc5,0xf8,0x2e,0x45,0x94,0x0f,0x97,0xc1,0xff,0xc3,0x85,0xdb,
        0x0f,0x9f,0xc2,0xb0,0x01,0x08,0xca,0x75,0x03,0x44,0x88,0xe8};
    static const unsigned char loop[11]={0x45,0x30,0xff,0xc5,0xf8,0x2e,0xc1,0xc5,0xf8,0x28,0xc8};
    unsigned char *at=image+build->update_stop, *l=image+build->update_loop;
    if (memcmp(at,stop,24) || at[24]!=0x41 || memcmp(l,loop,11) || l[11]!=0x0f || l[12]!=0x83) {
        printf("Frame rate: the update loop is not where eboot %s has it; below the frame rate the game slows down\n",build->version);
        return;
    }
    const uint64_t slot=(uint64_t)(uintptr_t)&updates;
    unsigned char *t=at;
    *t++=0xff; *t++=0xc3;                                   /* inc ebx */
    *t++=0x48; *t++=0xb8; memcpy(t,&slot,8); t+=8;          /* mov rax,&updates */
    *t++=0x89; *t++=0x58; *t++=0x04;                        /* mov [rax+4],ebx */
    *t++=0x3b; *t++=0x18;                                   /* cmp ebx,[rax] */
    *t++=0x0f; *t++=0x9d; *t++=0xc0;                        /* setge al */
    *t++=0x44; *t++=0x08; *t++=0xe8;                        /* or al,r13b */
    *t++=0x90;                                              /* nop */
    static const unsigned char until[11]={0x45,0x30,0xff, 0xc5,0xf8,0x28,0xc8, /* xor r15b,r15b; vmovaps xmm1,xmm0 */
        0x41,0xf6,0xc5,0x01};                                                 /* test r13b,1 */
    memcpy(l,until,11);
    l[12]=0x84;                                             /* jae loop -> je loop */
    updates_patched=1;
}

/* Cheats (settings.ini cheat_* keys, the cheat menu: L3+R3, deadnation_overlay.cpp), 01.03 only.
 * The session (global 01.03 +0x1bf6828, vtable +0x13c7c90) holds the score object at +0x2050 (its
 * vtable slot 0x78, 0x4749f0, returns it), which holds each player's record at +0x14 and +0x234:
 * the player's number (1, 2) at +0, cash at +4, and from +0x40 the inventory, 14 entries of 0x20
 * bytes by id (0 rifle, 1 SMG, 2 shotgun, 3 flamethrower, 4 blade cannon, 5 launcher, 6 shocker,
 * 7 flare, 8 grenade, 9 mine, 10 molotov, 11 dynamite, 12 auto-turret, 13 health): id at +0, state
 * at +4 (bit 0 in the shop, bit 1 owned), the levels of up to four upgrades at +8 and their highest
 * at +0x10 (0xff: no such upgrade; an entry with none is not in the game), the clip (an item's
 * count) at +0x18 and the spare ammo at +0x1a (int16; -1 unlimited, the rifle's). Sizes are the
 * weapon table's (+0xf536f0, 12 bytes an id: clip at +4, spare at +8, int16) scaled by an upgrade
 * (0x49cd20(entry, type): type 0 clip, 7 inventory), as the HUD and shop work them out. */
static const unsigned char god_site[7]={0x41,0x83,0x7f,0x04,0x00,0x74,0x15}; /* cmp dword [r15+4],0; je +0x15 */
static const unsigned char frame_prologue[4]={0x55,0x48,0x89,0xe5};
static const unsigned char armor_getter[7]={0x48,0x8b,0x87,0xa0,0x00,0x00,0x00}; /* mov rax,[rdi+0xa0] */
static const unsigned char list_getter[5]={0x83,0xfe,0x05,0x77,0x12};             /* cmp esi,5; ja */
static const unsigned char score_getter[8]={0x48,0x8b,0x87,0x50,0x20,0x00,0x00,0xc3}; /* mov rax,[rdi+0x2050]; ret */
static const int16_t weapon_clips[7]={16,40,4,100,5,5,100};
static const unsigned char timer_site[11]={0xc7,0x84,0xb0,0xd0,0,0,0,0,0,0,0}; /* mov dword [rax+rsi*4+0xd0],0 */
static int cheats_found;
static volatile unsigned char cheat_god, cheat_dash;

static int cheat_sites_found(const unsigned char *image) {
    if (!build || !build->session) return 0;
    for (int i=0;i<7;++i) {
        int16_t clip; memcpy(&clip,image+build->weapon_table+12*i+4,2);
        if (clip!=weapon_clips[i]) return 0;
    }
    return !memcmp(image+build->god_test,god_site,sizeof(god_site)) &&
           !memcmp(image+build->timer_start,timer_site,sizeof(timer_site)) &&
           !memcmp(image+build->score_getter,score_getter,sizeof(score_getter)) &&
           !memcmp(image+build->upgrade_scale,frame_prologue,4) &&
           !memcmp(image+build->armor_sets_getter,armor_getter,sizeof(armor_getter)) &&
           !memcmp(image+build->campaign_lists,list_getter,sizeof(list_getter)) &&
           !memcmp(image+build->road_lists,list_getter,sizeof(list_getter));
}

/* Invulnerability: the function that changes a character's health (01.03 0x494aa0, xmm0 the
 * change) leaves a player's (number at +0x10 up to 1) alone when it would lower it and the
 * character's god mode flag (+4, the scripts' VAR_GOD_MODE_ACTIVE) is set. The flag's test
 * (cmp dword [r15+4],0; je) goes to a stub that also takes the god mode branch while `cheat_god`
 * is set. */
static void patch_god(unsigned char *image, unsigned char *stub) {
    unsigned char *at=image+build->god_test, *t=stub;
    const uint64_t flag=(uint64_t)(uintptr_t)&cheat_god;
    *t++=0x50;                                                  /* push rax */
    *t++=0x48; *t++=0xb8; memcpy(t,&flag,8); t+=8;              /* mov rax,&cheat_god */
    *t++=0x80; *t++=0x38; *t++=0x00;                            /* cmp byte [rax],0 */
    *t++=0x58;                                                  /* pop rax */
    *t++=0x75; *t++=7;                                          /* jne god */
    memcpy(t,god_site,5); t+=5;                                 /* cmp dword [r15+4],0 */
    *t++=0x74; *t++=5;                                          /* je no_god */
    int32_t rel=(int32_t)((at+7)-(t+5));
    *t++=0xe9; memcpy(t,&rel,4); t+=4;                          /* god: jmp site+7 */
    rel=(int32_t)((at+7+god_site[6])-(t+5));
    *t++=0xe9; memcpy(t,&rel,4);                                /* no_god: jmp the je's target */
    rel=(int32_t)(stub-(at+5));
    at[0]=0xe9; memcpy(at+1,&rel,4); at[5]=at[6]=0x90;          /* jmp stub */
}

/* Dash recovers immediately: a character's script timers are four slots in its record (elapsed
 * at +0xd0, length at +0xe0, tag at +0xf0); dashing starts the one tagged with the hash the game
 * keeps at +0x18b1098 (the HUD's dash bar shows it), 3.6 s, and dash waits for it to run out. The
 * timer start (01.03 0x490ad0) zeroes the elapsed time at 0x490b27; a stub stores the length
 * there instead, the timer already run out, for that tag while `cheat_dash` is set. */
static void patch_dash(unsigned char *image, unsigned char *stub) {
    unsigned char *at=image+build->timer_start, *t=stub;
    const uint64_t flag=(uint64_t)(uintptr_t)&cheat_dash, tag=(uint64_t)(uintptr_t)(image+build->rush_tag);
    *t++=0x49; *t++=0xbb; memcpy(t,&flag,8); t+=8;              /* mov r11,&cheat_dash */
    *t++=0x41; *t++=0x80; *t++=0x3b; *t++=0x00;                 /* cmp byte [r11],0 */
    *t++=0x74; *t++=29;                                         /* je zero */
    *t++=0x49; *t++=0xbb; memcpy(t,&tag,8); t+=8;               /* mov r11,&rush tag */
    *t++=0x41; *t++=0x3b; *t++=0x13;                            /* cmp edx,[r11] (the timer's tag) */
    *t++=0x75; *t++=14;                                         /* jne zero */
    static const unsigned char done[9]={0xc5,0xfa,0x11,0x84,0xb0,0xd0,0,0,0}; /* vmovss [rax+rsi*4+0xd0],xmm0 */
    memcpy(t,done,sizeof(done)); t+=sizeof(done);
    int32_t rel=(int32_t)((at+11)-(t+5));
    *t++=0xe9; memcpy(t,&rel,4); t+=4;                          /* jmp site+11 */
    memcpy(t,timer_site,sizeof(timer_site)); t+=sizeof(timer_site); /* zero: the replaced store */
    rel=(int32_t)((at+11)-(t+5));
    *t++=0xe9; memcpy(t,&rel,4);                                /* jmp site+11 */
    rel=(int32_t)(stub-(at+5));
    at[0]=0xe9; memcpy(at+1,&rel,4); memset(at+5,0x90,6);       /* jmp stub */
}

/* The score object, or NULL when the session is not up (or not the one this build has). */
static unsigned char *cheat_score(void) {
    unsigned char *session=*(unsigned char **)(game_image+build->session);
    if (!session || *(unsigned char **)session!=game_image+build->session_vtable) return NULL;
    return *(unsigned char **)(session+0x2050);
}

/* Unlock all armors, as the debug menu's "Unlock armors" (01.03 0xe30c40) does for the campaign,
 * and for Arcade (Road of Devastation) too. The armor sets (global +0x1bf6898, vtable +0x13ce1d0;
 * slot 0x38 returns their array, begin and end, 0x7c bytes each, id at +0) are the pieces of every
 * slot. The profile (global +0x1bf67f8, vtable +0x13c53b0) keeps a list of the unlocked ones per
 * slot (kind 0 arms, 1 torso, 2 legs): slot 0x98 gives the campaign's, slot 0xa0 Road of
 * Devastation's (the armor screen picks by mode, 0x5311b2). A list copies out to a set of up to 64
 * ids ending at a 0 (its vtable slot 0, set's add 0x48ac30) and takes one back (slot 1). Run when
 * the cheat is on as the main menu or a game starts. */
static void unlock_armor(void) {
    unsigned char *sets=*(unsigned char **)(game_image+build->armor_sets), *profile=*(unsigned char **)(game_image+build->profile);
    if (!sets || !profile || *(unsigned char **)sets!=game_image+build->armor_sets_vtable ||
        *(unsigned char **)profile!=game_image+build->profile_vtable) return;
    void **sets_vtable=*(void ***)sets, **profile_vtable=*(void ***)profile;
    if (sets_vtable[0x38/8]!=game_image+build->armor_sets_getter || profile_vtable[0x98/8]!=game_image+build->campaign_lists ||
        profile_vtable[0xa0/8]!=game_image+build->road_lists) return;
    typedef struct { const unsigned char *begin, *end; } Range;
    typedef Range (ABI *SetsOf)(void *self);
    typedef void *(ABI *ListOf)(void *self, int32_t kind);
    typedef void (ABI *Copy)(void *to, void *from);
    const Range all=((SetsOf)sets_vtable[0x38/8])(sets);
    int added=0;
    for (int mode=0;mode<2;++mode)
        for (int32_t kind=0;kind<3;++kind) {
            void *list=((ListOf)profile_vtable[(mode ? 0xa0 : 0x98)/8])(profile,kind);
            void **list_vtable=*(void ***)list;
            uint32_t ids[128]={0};
            ((Copy)list_vtable[0])(ids,list);
            for (const unsigned char *set=all.begin;set && set+0x7c<=all.end;set+=0x7c) {
                uint32_t id; memcpy(&id,set,4);
                int at=0;
                while (at<64 && ids[at] && ids[at]!=id) ++at;
                if (at<64 && !ids[at] && id) { ids[at]=id; if (at+1<64) ids[at+1]=0; ++added; }
            }
            ((Copy)list_vtable[1])(list,ids);
        }
    if (added) printf("Cheats: %d armor pieces unlocked (campaign and Arcade)\n",added);
}

/* At each flip, on the game's thread (the one that runs the game's updates). */
static void apply_cheats(const char *screen_changed) {
    const unsigned on=deadnation_cheats();
    cheat_god=(on&DEADNATION_CHEAT_INVULNERABLE)!=0;
    cheat_dash=(on&DEADNATION_CHEAT_DASH)!=0;
    if (!cheats_found) return;
    static unsigned last;
    static int armor_screen;
    if (screen_changed) armor_screen=!strcmp(screen_changed,"Screen_Main_Menu") || !strcmp(screen_changed,"Screen_Game");
    if ((on&DEADNATION_CHEAT_ARMOR) && armor_screen && (screen_changed || !(last&DEADNATION_CHEAT_ARMOR))) unlock_armor();
    if (on!=last) printf("Cheats: %#x\n",on);
    last=on;
    if (!(on&~(unsigned)(DEADNATION_CHEAT_INVULNERABLE|DEADNATION_CHEAT_ARMOR|DEADNATION_CHEAT_DASH))) return;
    unsigned char *score=cheat_score();
    if (!score) return;
    typedef float (ABI *Scale)(void *entry, int32_t type);
    const Scale scale=(Scale)(game_image+build->upgrade_scale);
    const unsigned char *table=game_image+build->weapon_table;
    for (int32_t p=0;p<2;++p) {
        unsigned char *player=score+0x14+0x220*p;
        if (*(int32_t *)player!=p+1) continue;
        int32_t *cash=(int32_t *)(player+4);
        if ((on&DEADNATION_CHEAT_MONEY) && *cash<9999999) *cash=9999999;
        for (int32_t id=0;id<13;++id) {
            unsigned char *e=player+0x40+0x20*id, *level=e+8, *top=e+0x10;
            if (*(int32_t *)e!=id || (top[0]==0xff && top[1]==0xff && top[2]==0xff && top[3]==0xff)) continue;
            const int item=id>=7;
            int32_t *state=(int32_t *)(e+4);
            int16_t clip, spare;
            memcpy(&clip,e+0x18,2); memcpy(&spare,e+0x1a,2);
            int given=0;
            if ((on&(item ? DEADNATION_CHEAT_ALL_ITEMS : DEADNATION_CHEAT_ALL_WEAPONS)) && !(*state&2)) { *state|=3; given=1; }
            if (on&DEADNATION_CHEAT_WEAPON_UPGRADES)
                for (int k=0;k<4;++k) if (top[k]!=0xff && level[k]<top[k]) level[k]=top[k];
            if (!(*state&2)) continue;
            int16_t base_clip, base_spare;
            memcpy(&base_clip,table+12*id+4,2); memcpy(&base_spare,table+12*id+8,2);
            const int16_t full_clip=(int16_t)(base_clip*scale(e,0)), full_spare=base_spare<0 ? -1 : (int16_t)(base_spare*scale(e,7));
            if (given && clip<=0 && spare<=0) { clip=full_clip; if (full_spare>0) spare=full_spare; }
            if (!item && (on&DEADNATION_CHEAT_NO_RELOAD) && clip<full_clip) clip=full_clip;
            if (!item && (on&DEADNATION_CHEAT_AMMO) && full_spare>0 && spare<full_spare) spare=full_spare;
            if (item && (on&DEADNATION_CHEAT_ITEMS) && clip<full_clip) clip=full_clip;
            memcpy(e+0x18,&clip,2); memcpy(e+0x1a,&spare,2);
        }
    }
}

void runtime_game_patch(unsigned char *image, unsigned char *patches) {
    if (!build) { puts("Screens: an eboot.bin the runner does not know; skip_intro is off"); return; }
    if (memcmp(image+build->screen_push,screen_prologue,6) || memcmp(image+build->screen_pop,screen_prologue,6)) {
        printf("Screens: the screen manager is not where eboot %s has it; skip_intro is off\n",build->version);
        build=NULL;
        return;
    }
    unsigned char *t=hook_entry(image,build->screen_pop,hook_entry(image,build->screen_push,patches+0x100));
    t=patch_logic(image,t);
    patch_updates(image);
    patch_render_size(image,t);
    if (cameras_found(image)) patch_view_rect(image,patch_camera(image,t+64));
    if (cheat_sites_found(image)) { patch_god(image,patches+0xe00); patch_dash(image,patches+0xe80); cheats_found=1; }
    else printf("Cheats: eboot %s does not have them where the runner looks; the cheat menu does nothing\n",build->version);
}

/* The name of the screen showing, or NULL before the first screen change. Called on the game's
 * thread (at its flips), the thread that changes the stack. */
const char *runtime_game_screen(void) {
    unsigned char *manager=screen_manager;
    if (!build || !manager) return NULL;
    void **begin=*(void ***)(manager+0xa8), **end=*(void ***)(manager+0xb0);
    if (!begin || end<=begin || end-begin>64) return NULL;
    void *top=end[-1];
    if (!top) return NULL;
    void **vtable=*(void ***)top;
    typedef const char *(ABI *Name)(void *self);
    return ((Name)vtable[2])(top);
}

/* settings.ini frame_rate onto the engine's frame clock. The main loop (01.03 0xcafe70) measures
 * the real time since the last frame and runs one update a frame (0xcaf980) of the clock's step:
 * the game sets 30 at boot, so it ran at 30 whatever the vblank (patch_updates). The clock is a global
 * object (01.03 +0x1bf6958, 01.00 +0x1bf26f8); set_fps(float) is vtable slot 3 (target at +8,
 * step at +0xc; it also sets the display rate at +0x10/+0x14). Applied at each flip, which also
 * restores the rate after the game resets it. Before the first change the clock is checked: a
 * vtable in the image's data, set_fps in its code and a target near 30 or 60 (until the game
 * sets one, the target is FLT_MIN). */
static int clock_state; /* 0 unchecked, 1 good, -1 not the clock this build has */
static void apply_frame_rate(void) {
    if (!game_image || !build || clock_state<0) return;
    float *clock=*(float **)(game_image+build->clock);
    if (!clock) return;
    void **vtable=*(void ***)clock;
    const float target=clock[2];
    if (!clock_state) {
        if (target<1.0f) return; /* not set yet */
        const uintptr_t v=(uintptr_t)vtable-(uintptr_t)game_image, set=(uintptr_t)vtable[3]-(uintptr_t)game_image;
        if (v<build->data || v>=build->data_end || set<0x400000 || set>=build->code_end ||
            !((target>59.0f && target<60.5f) || (target>29.5f && target<30.5f))) {
            clock_state=-1;
            deadnation_set_frame_rate(30); /* the game's own rate */
            printf("Frame rate: the engine's frame clock is not where eboot %s has it (vtable +%#zx, "
                   "target %.2f); the game keeps its own 30 FPS\n",build->version,(size_t)v,(double)target);
            return;
        }
        clock_state=1;
        printf("Frame rate: engine clock found (game's target %.0f FPS)\n",(double)target);
    }
    const int rate=deadnation_frame_rate();
    if (target==(float)rate) return;
    typedef void (ABI *SetFps)(void *clock, float fps);
    ((SetFps)vtable[3])(clock,(float)rate);
    printf("Frame rate: %d FPS (settings.ini frame_rate)\n",rate);
}

/* Game speed check, at each flip: `dt` the real time since the last flip, `ran` the game time the
 * frame's updates covered (updates run times their step). Over windows of 2 s, logs the game's
 * speed against real time, the average FPS and the cap, only when it is off by more than 5% (at
 * 30 FPS and up, patch_updates keeps it at 100%). Frames over 100 ms (loading, a stall the PS4
 * drops too) are left out of both sums, so loads do not show as slowdowns. */
static void speed_check(double dt, double ran, int rate) {
    static double real, game;
    static int frames;
    if (dt>0.1) return;
    real+=dt; game+=ran; ++frames;
    if (real<2.0) return;
    const double speed=game/real;
    if (speed<0.95 || speed>1.05)
        printf("Speed: game at %.0f%% of real time (average %.1f FPS, cap %d FPS)\n",
               speed*100.0,frames/real,rate);
    real=game=0.0; frames=0;
}

/* The updates the next frame runs (patch_updates): as many steps of 1/fps as the real time since
 * the last flip holds, at least 1 and at most fps/30 (down to 30 FPS the game keeps real time;
 * below, it slows, as on the PS4). `owed` carries what is left, and what one update gave a quick
 * frame (flips jitter around 1/fps), so on time it is one update a frame, as before. Also picks
 * which of them tick the item scripts, 30 a second (patch_logic). settings.ini debug_speed (2 to 4)
 * runs that many times the game time per real second: as many more fixed steps a frame (at most
 * 32, tick_mask's bits), item ticks included, so the game runs as at 1x, only faster. */
static void update_count(void) {
    static uint64_t last;
    static double owed, ticks, last_step;
    static int shown_speed=1;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    const uint64_t ns=(uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
    const int rate=clock_state>0 ? deadnation_frame_rate() : 30;
    const int speed=updates_patched && clock_state>0 ? deadnation_debug_speed() : 1;
    if (speed!=shown_speed) {
        printf("Debug: game speed %dx (settings.ini debug_speed)\n",speed);
        shown_speed=speed;
    }
    const double step=1.0/rate;
    int most=(rate/30>1 ? rate/30 : 1)*speed;
    if (most>32) most=32;
    if (last) {
        const int ran=updates_patched ? updates.done : 1; /* unpatched, the loop runs one a frame */
        owed+=(double)(ns-last)*1e-9*speed;
        speed_check((double)(ns-last)*1e-9,ran*last_step/speed,rate);
    }
    last_step=step;
    last=ns;
    int n=(int)(owed/step+0.01);
    n=n<1 ? 1 : n>most ? most : n;
    owed-=n*step;
    if (owed>step) owed=step; /* more than fps/30 updates owed: dropped, as the PS4 drops it */
    if (owed<-step) owed=-step;
    if (!updates_patched) n=1;
    uint32_t mask=0;
    for (int i=0;i<n;++i) {
        ticks+=rate>30 ? 30.0/rate : 1.0;
        if (ticks>=0.999) { mask|=1u<<i; ticks-=1.0; }
    }
    tick_mask=mask;
    logic_tick=mask&1;
    updates.wanted=n;
    updates.done=0;
}

/* At each flip, on the game's thread: the frame rate, the next frame's updates, and screen
 * changes (logged, and told to skip_intro in runtime_pad.c). */
void runtime_game_frame_rate(void) {
    /* The cheat menu pauses the game: its thread waits here, before the frame's flip, while the
     * presenter shows the last frame under the menu (videoout driver.cpp). */
    if (deadnation_cheat_menu_open()) {
        const struct timespec wait={0,10000000};
        while (deadnation_cheat_menu_open()) nanosleep(&wait,NULL);
    }
    apply_frame_rate();
    update_count();
    static const char *last;
    const char *screen=runtime_game_screen(), *changed=NULL;
    if (screen && screen!=last) {
        last=changed=screen;
        printf("Screen: %s\n",screen);
        runtime_pad_intro_screen(screen);
    }
    apply_cheats(changed);
}

void runtime_game_set_image(unsigned char *image) { game_image=image; }

/* A 4:3, 16:10, 21:9 or 32:9 output: patch_render_size gives the render device the output's shape
 * (deadnation_render_width/height: wider for 21:9 and 32:9, taller for 4:3 and 16:10, which also
 * need patch_camera and patch_view_rect). Otherwise the game keeps 16:9 and the presenter shows
 * bars. */
int runtime_game_widen(unsigned char *image, unsigned char *patches, uint32_t width_4k) {
    (void)patches;
    if (!render_size_found(image)) { puts("Render: the render device is not where this eboot has it; other aspect ratios show 16:9 with bars"); return 0; }
    if (width_4k<3840 && !cameras_found(image)) { puts("Render: the cameras are not where this eboot has them; 4:3 and 16:10 show 16:9 with bars"); return 0; }
    return 1;
}
