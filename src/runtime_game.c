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
#include "gpu/deadnation_gpu.h"

/* eboot.bin builds the runner has been run with, and in each: the screen manager's two methods
 * that change its stack of screens (push(manager, screen) and pop-to(manager, ...)), the global
 * holding the frame clock, the end of the code and the data segment holding the clock's vtable.
 * The setup warns about other builds: keep kTested* in setup/setup.cpp in step. */
typedef struct {
    const char *sha256, *version;
    uint32_t screen_push, screen_pop, clock, code_end, data, data_end, render_size, persp_call, ortho_call, finalize, view_rect, ortho_aspect, view_fov;
} Build;
static const Build known_builds[]={
    {"09b367a222cfb6308811650ae1fb8105ea6c7726137646907b211419027aa37c","01.03",
     0xe21940,0xe21150,0x1bf6958,0x13b4920,0x13b8000,0x1bf6ef0,0xc88010,0xce352a,0xce3658,0xce2cf0,0xcebbd1,0xceb3db,0xddadd7},
    {"8d2ae241ca42585d0b2601554720f198a7ef63c0b6f80fc34325883530ab8135","01.00",
     0xe1f600,0xe1f4b0,0x1bf26f8,0x13b2418,0x13b4000,0x1bf2c90,0xc866d0,0xce17aa,0xce18d8,0xce0f70,0xce9e51,0xce965b,0xdd9837},
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
 * push r15) and jumps back. */
static void *volatile screen_manager;
static const unsigned char screen_prologue[6]={0x55,0x48,0x89,0xe5,0x41,0x57};

static unsigned char *hook_entry(unsigned char *image, uint32_t at, unsigned char *stub) {
    unsigned char *t=stub;
    const uint64_t slot=(uint64_t)(uintptr_t)&screen_manager;
    *t++=0x50;                                                  /* push rax */
    *t++=0x48; *t++=0xb8; memcpy(t,&slot,8); t+=8;              /* mov rax,&screen_manager */
    *t++=0x48; *t++=0x89; *t++=0x38;                            /* mov [rax],rdi */
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

void runtime_game_patch(unsigned char *image, unsigned char *patches) {
    if (!build) { puts("Screens: an eboot.bin the runner does not know; skip_intro is off"); return; }
    if (memcmp(image+build->screen_push,screen_prologue,6) || memcmp(image+build->screen_pop,screen_prologue,6)) {
        printf("Screens: the screen manager is not where eboot %s has it; skip_intro is off\n",build->version);
        build=NULL;
        return;
    }
    unsigned char *t=hook_entry(image,build->screen_pop,hook_entry(image,build->screen_push,patches+0x100));
    patch_render_size(image,t);
    if (cameras_found(image)) patch_view_rect(image,patch_camera(image,t+64));
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
 * the real time since the last frame and runs the game's update in fixed steps of the clock's
 * 1/fps (0xcaf980, at most 8 a frame), waiting out the rest of a step: the game sets 30 at boot,
 * so it ran at 30 whatever the vblank. Its speed is real time at any rate. The clock is a global
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

/* At each flip, on the game's thread: the frame rate, and screen changes (logged, and told to
 * skip_intro in runtime_pad.c). */
void runtime_game_frame_rate(void) {
    apply_frame_rate();
    static const char *last;
    const char *screen=runtime_game_screen();
    if (screen && screen!=last) {
        last=screen;
        printf("Screen: %s\n",screen);
        runtime_pad_intro_screen(screen);
    }
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
