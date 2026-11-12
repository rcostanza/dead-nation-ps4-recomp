/* PSN libraries for the offline console (runtime_services.c holds NpManager, the older NpScore,
 * Matching2 and WebApi calls, and the trophies). The one user is signed out, so (shadPS4 np_*):
 *   - creating a title, user or matching context fails with NP SIGNED_OUT;
 *   - a call on a context or request that can therefore not exist fails with that library's
 *     INVALID_ID; a request object the old API created fails its transfer with SIGNED_OUT;
 *   - objects that need no sign-in (WebApi handles and push filters, InGameMessage contexts,
 *     NpAuth requests) are created, and their server calls fail with SIGNED_OUT.
 * Dialogs about PSN or the system (sign-in, invitation, error, social screen) draw nothing: an
 * opened one finishes at once, cancelled by the user. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define COMMUNITY_INVALID_ARGUMENT ((int32_t)0x80550704)
#define COMMUNITY_INVALID_ID ((int32_t)0x8055070E)
#define AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define MATCHING2_INVALID_ARGUMENT ((int32_t)0x80550C0A)
#define MATCHING2_INVALID_CONTEXT_ID ((int32_t)0x80550C0B)
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_INVALID_LIB_CONTEXT_ID ((int32_t)0x80552903)
#define INVALID_SERVICE_LABEL 0xFFFFFFFFu
#define USER_INVALID (-1)

static _Atomic int32_t next_object=1;
static int32_t new_object(void) { return atomic_fetch_add(&next_object,1); }
static ABI int32_t ok(void) { return 0; }
static ABI int32_t signed_out(void) { return NP_SIGNED_OUT; }

/* ---- NpTus / NpTss (shadPS4 np_tus.cpp) ----
 * sceNpTusCreateNpTitleCtxA(label, user) and sceNpTssCreateNpTitleCtxA need the user's NpId:
 * SIGNED_OUT. With no title context, sceNpTusCreateRequest(ctx), every Get/Set/Add/TryAndSet/
 * Delete call on a request, sceNpTusAbortRequest, sceNpTusDeleteRequest and
 * sceNpTusDeleteNpTitleCtx find no such id: COMMUNITY INVALID_ID. */
static ABI int32_t community_no_id(void) { return COMMUNITY_INVALID_ID; }

/* ---- NpScore A variants (shadPS4 np_score.cpp) ----
 * sceNpScoreCreateNpTitleCtxA(label, user): SIGNED_OUT first, then the label is checked. The
 * ranking and game-data calls take a request from sceNpScoreCreateRequest and fail SIGNED_OUT,
 * as the older calls in runtime_services.c do. */
static ABI int32_t score_ctx_a(uint32_t label,int32_t user) { (void)label; (void)user; return NP_SIGNED_OUT; }

/* ---- NpMatching2 (shadPS4 np_matching2.cpp) ----
 * sceNpMatching2CreateContextA(param{userId, serviceLabel, size=0x10}, ctxId): INVALID_ARGUMENT
 * for a bad param, else SIGNED_OUT. Every call that names a context then fails with
 * INVALID_CONTEXT_ID. sceNpMatching2GetMemoryInfo / GetSslMemoryInfo: OK, as shadPS4. */
static ABI int32_t matching2_context_a(const uint8_t *param,uint16_t *ctx) {
    if (!param || !ctx) return MATCHING2_INVALID_ARGUMENT;
    uint64_t size; memcpy(&size,param+8,8);
    return size!=0x10 ? MATCHING2_INVALID_ARGUMENT : NP_SIGNED_OUT;
}
static ABI int32_t matching2_no_context(void) { return MATCHING2_INVALID_CONTEXT_ID; }

/* ---- NpWebApi (shadPS4 np_web_api.cpp) ----
 * sceNpWebApiCreateContextA(libCtx, user): a user context needs PSN: SIGNED_OUT (as
 * sceNpWebApiCreateContext). Handles and extended push-event filters belong to the library
 * context and are created; requests and push callbacks need a user context: SIGNED_OUT. */
static ABI int32_t webapi_context_a(int32_t lib,int32_t user) {
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    return user==USER_INVALID ? WEBAPI_INVALID_ARGUMENT : NP_SIGNED_OUT;
}
static ABI int32_t webapi_handle(int32_t lib) { return lib>=0x8000 ? WEBAPI_INVALID_LIB_CONTEXT_ID : new_object(); }
/* sceNpWebApiCreateExtdPushEventFilter(lib, handle, service name, label, params, count). */
static ABI int32_t webapi_filter(int32_t lib,int32_t handle,const char *service,uint32_t label,const void *params,uint64_t count) {
    (void)handle;
    if ((service && label==INVALID_SERVICE_LABEL) || !params || !count) return WEBAPI_INVALID_ARGUMENT;
    return lib>=0x8000 ? WEBAPI_INVALID_LIB_CONTEXT_ID : new_object();
}
/* sceNpWebApiRegisterExtdPushEventCallbackA(userCtx, filter, callback, arg). */
static ABI int32_t webapi_callback_a(int32_t ctx,int32_t filter,void *callback,void *arg) {
    (void)ctx; (void)filter; (void)arg; return callback ? NP_SIGNED_OUT : WEBAPI_INVALID_ARGUMENT;
}

/* ---- NpUtility (shadPS4 np_utility.cpp) ----
 * sceNpWordFilterCreateTitleCtxA(user): SIGNED_OUT; the request and sanitize calls then name a
 * context that does not exist: COMMUNITY INVALID_ID. The bandwidth test measures the PSN
 * connection: sceNpBandwidthTestInitStart / GetStatus / Shutdown fail SIGNED_OUT, Abort is OK. */

/* ---- NpManager InGameMessage ----
 * sceNpInGameMessageInitialize(pool size, option) gives a library context and
 * sceNpInGameMessageCreateHandle(ctx) a handle; PrepareA and SendDataA send to other players:
 * SIGNED_OUT. Delete, Terminate and the pool statistics are OK. */
static ABI int32_t object(void) { return new_object(); }

/* ---- NpAuth (shadPS4 np_auth.cpp) ----
 * sceNpAuthCreateRequest: a synchronous request id (0x10000001 up). sceNpAuthGetAuthorizationCodeA
 * (req, param, code, issuer) and sceNpAuthGetIdTokenA(req, param, token) need param and output,
 * then fail SIGNED_OUT. sceNpAuthAbortRequest: OK. */
static _Atomic int32_t next_auth=0x10000001;
static ABI int32_t auth_request(void) { return atomic_fetch_add(&next_auth,1); }
static ABI int32_t auth_code_a(int32_t req,const void *param,void *code,int32_t *issuer) {
    (void)req; (void)issuer; return !param || !code ? AUTH_INVALID_ARGUMENT : NP_SIGNED_OUT;
}

/* SDK 1-3 forms, which take an NpId / OnlineId where the A forms take a user: the same failures.
 * sceNpAuthCreateAsyncRequest gives a request whose sceNpAuthGetAuthorizationCode fails at once;
 * sceNpAuthPollAsync(req, result) then reports it finished (0) with that error. */
static ABI int32_t auth_poll(int32_t req,int32_t *result) { (void)req; if (result) *result=NP_SIGNED_OUT; return 0; }
/* sceNpCmpOnlineId(a, b): 0 when the 16-byte ids match, else NOT_MATCH. */
static ABI int32_t cmp_online_id(const char *a,const char *b) {
    if (!a || !b) return (int32_t)0x80550003;
    return strncmp(a,b,16) ? (int32_t)0x80550609 : 0;
}
/* ---- NpCommerce: sceNpCommerceShowPsStoreIcon(pos) / HidePsStoreIcon: OK, no icon is drawn
 * (the Store is unreachable offline). ---- */

/* ---- Dialogs: status 0 none, 1 initialized, 2 running, 3 finished ---- */
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define DIALOG_ALREADY_INITIALIZED ((int32_t)0x80B80004)
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define RESULT_USER_CANCELED 1
enum { SIGNIN, INVITATION, ERROR_DIALOG, SOCIAL_SCREEN, FRIEND_LIST, DIALOGS };
static const char *const dialog_names[DIALOGS]={"SigninDialog","InvitationDialog","ErrorDialog","SocialScreenDialog","NpFriendListDialog"};
static _Atomic int dialog_status[DIALOGS];
static int32_t dialog_init(int d) {
    int none=0;
    return atomic_compare_exchange_strong(&dialog_status[d],&none,1) ? 0 : DIALOG_ALREADY_INITIALIZED;
}
static int32_t dialog_open(int d,const char *detail) {
    if (!atomic_load(&dialog_status[d])) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened%s; finished at once (offline, no dialog UI)\n",dialog_names[d],detail);
    atomic_store(&dialog_status[d],3); return 0;
}
static int32_t dialog_result(int d,void *result) {
    if (atomic_load(&dialog_status[d])!=3) return DIALOG_NOT_FINISHED;
    return result ? 0 : DIALOG_ARG_NULL;
}
static int32_t dialog_term(int d) {
    if (!atomic_load(&dialog_status[d])) return DIALOG_NOT_INITIALIZED;
    atomic_store(&dialog_status[d],0); return 0;
}
/* sceInvitationDialog*: nothing can be sent. OrbisInvitationDialogResultA: callbackArg @0 (kept),
 * errorCode @8, result @12, sentUsers @16, reserved[32]. */
static ABI int32_t invitation_init(void) { return dialog_init(INVITATION); }
static ABI int32_t invitation_open(const void *param) { (void)param; return dialog_open(INVITATION,""); }
static ABI int32_t invitation_status(void) { return atomic_load(&dialog_status[INVITATION]); }
static ABI int32_t invitation_result(uint8_t *result) {
    int32_t r=dialog_result(INVITATION,result);
    if (r) return r;
    int32_t codes[2]={NP_SIGNED_OUT,RESULT_USER_CANCELED};
    memcpy(result+8,codes,8); memset(result+16,0,40);
    return 0;
}
static ABI int32_t invitation_term(void) { return dialog_term(INVITATION); }
/* sceErrorDialog*: param {int32 size, int32 errorCode, ...}; the code goes to the log. */
static ABI int32_t error_init(void) { return dialog_init(ERROR_DIALOG); }
static ABI int32_t error_open(const int32_t *param) {
    if (!param) return DIALOG_ARG_NULL;
    char detail[48]; snprintf(detail,sizeof(detail)," (error 0x%08x)",(uint32_t)param[1]);
    return dialog_open(ERROR_DIALOG,detail);
}
static ABI int32_t error_status(void) { return atomic_load(&dialog_status[ERROR_DIALOG]); }
static ABI int32_t error_term(void) { return dialog_term(ERROR_DIALOG); }
/* sceInvitationDialogClose / sceErrorDialogClose: as sceSocialScreenDialogClose. */
static ABI int32_t invitation_close(void) { return atomic_load(&dialog_status[INVITATION]) ? 0 : DIALOG_NOT_INITIALIZED; }
static ABI int32_t error_close(void) { return atomic_load(&dialog_status[ERROR_DIALOG]) ? 0 : DIALOG_NOT_INITIALIZED; }
/* sceNpFriendListDialog*: no friends offline; the player leaves it. The result's first field is
 * the result (USER_CANCELED). */
static ABI int32_t friends_init(void) { return dialog_init(FRIEND_LIST); }
static ABI int32_t friends_open(const void *param) { (void)param; return dialog_open(FRIEND_LIST,""); }
static ABI int32_t friends_status(void) { return atomic_load(&dialog_status[FRIEND_LIST]); }
static ABI int32_t friends_result(int32_t *result) {
    int32_t r=dialog_result(FRIEND_LIST,result);
    if (!r) *result=RESULT_USER_CANCELED;
    return r;
}
static ABI int32_t friends_close(void) { return atomic_load(&dialog_status[FRIEND_LIST]) ? 0 : DIALOG_NOT_INITIALIZED; }
static ABI int32_t friends_term(void) { return dialog_term(FRIEND_LIST); }

/* ---- Older NpManager, NpLookup and NpTus calls (Dead Nation, SDK 1.x) ----
 * The user's online id and country need PSN: SIGNED_OUT. sceNpCreateRequest makes a request id
 * (the old API's, whose transfers fail SIGNED_OUT). sceNpLookupCreateTitleCtx and
 * sceNpTusCreateNpTitleCtx need the user's NpId: SIGNED_OUT; calls on the contexts and requests
 * that therefore do not exist: COMMUNITY INVALID_ID. */
static ABI int32_t np_request(void) { return new_object(); }
/* sceNpCmpNpId(a, b): 0 when the two NpIds (online id handle and its options) name the same
 * user, NOT_MATCH otherwise. */
#define NP_UTIL_NOT_MATCH ((int32_t)0x80550609)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
static ABI int32_t np_cmp_np_id(const uint8_t *a,const uint8_t *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return strncmp((const char *)a,(const char *)b,16) || memcmp(a+20,b+20,8) ? NP_UTIL_NOT_MATCH : 0;
}

/* ---- GameLiveStreaming (shadPS4 game_live_streaming.cpp) ----
 * Broadcasting from the PS4 share menu: never on air, no spectators, no social feedback. The
 * settings calls are accepted. */
#define GLS_NOT_AVAILABLE ((int32_t)0x80A10002)
static ABI int32_t gls_status(uint8_t *status) {
    if (!status) return (int32_t)0x80A10003;
    memset(status,0,12); /* isOnAir, spectatorCounts, userId */
    return 0;
}
static ABI int32_t gls_message_count(int32_t type,uint32_t *count) { (void)type; if (count) *count=0; return 0; }
static ABI int32_t gls_not_available(void) { return GLS_NOT_AVAILABLE; }

/* ---- Voice (shadPS4 voice.cpp) ----
 * Voice chat for online play, which never starts: the library and its ports work, no microphone
 * ever gives data (ReadFromOPort reads 0 bytes) and what the game writes is dropped. */
static _Atomic uint32_t next_port=1;
static ABI int32_t voice_create_port(uint32_t *port,const void *param) {
    if (!port || !param) return (int32_t)0x8023ba01;
    *port=atomic_fetch_add(&next_port,1); return 0;
}
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }

/* ---- libSceXml (sce::Xml) ----
 * An Initializer object the game keeps; its constructor, destructor and initialize do nothing
 * the runner needs (the parse calls, if a path ever reaches them, stop the run). */
static ABI void xml_this(void *self) { (void)self; }
static ABI int32_t xml_initialize(void *self,const void *param) { (void)self; (void)param; return 0; }

/* ---- GameCustomDataDialog ----
 * Shows a PSN message's attached game data (invitations): no message reaches an offline console.
 * Open is accepted and the dialog finishes at once (status FINISHED). */
static ABI int32_t custom_dialog_status(void) { return 3; }

static const RuntimeExport exports[]={
    {"sceNpGetOnlineId",signed_out}, {"sceNpGetAccountCountry",signed_out}, {"sceNpCreateRequest",np_request},
    {"sceNpCmpNpId",np_cmp_np_id},
    {"sceNpLookupCreateTitleCtx",signed_out}, {"sceNpLookupDeleteTitleCtx",community_no_id},
    {"sceNpLookupDeleteRequest",community_no_id}, {"sceNpLookupNpId",community_no_id},
    {"sceNpLookupPollAsync",community_no_id},
    {"sceNpTusCreateNpTitleCtx",signed_out}, {"sceNpTusAddAndGetVariableVUserAsync",community_no_id},
    {"sceNpTusGetMultiSlotVariableVUserAsync",community_no_id}, {"sceNpTusGetDataAsync",community_no_id},
    {"sceNpTusSetDataAsync",community_no_id},
    {"sceNpMatching2GetRoomDataInternal",matching2_no_context},
    {"sceNpMatching2SignalingGetConnectionInfo",matching2_no_context},
    {"sceNpMatching2SignalingGetLocalNetInfo",matching2_no_context},
    {"sceNpWebApiSendRequest",signed_out}, {"sceNpWebApiGetHttpStatusCode",signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValue",signed_out},
    {"sceGameLiveStreamingInitialize",ok}, {"sceGameLiveStreamingTerminate",ok},
    {"sceGameLiveStreamingGetCurrentStatus",gls_status},
    {"sceGameLiveStreamingGetSocialFeedbackMessagesCount",gls_message_count},
    {"sceGameLiveStreamingGetSocialFeedbackMessages",gls_not_available},
    {"sceGameLiveStreamingGetProgramInfo",gls_not_available},
    {"sceGameLiveStreamingPermitServerSideRecording",ok}, {"sceGameLiveStreamingEnableLiveStreaming",ok},
    {"sceGameLiveStreamingEnableSocialFeedback",ok}, {"sceGameLiveStreamingPostSocialMessage",ok},
    {"sceGameLiveStreamingSetMetadata",ok}, {"sceGameLiveStreamingSetSpoilerTag",ok},
    {"sceGameLiveStreamingClearSpoilerTag",ok}, {"sceGameLiveStreamingSetPresetSocialFeedbackCommands",ok},
    {"sceGameLiveStreamingClearPresetSocialFeedbackCommands",ok},
    {"sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription",ok},
    {"sceVoiceInit",ok}, {"sceVoiceEnd",ok}, {"sceVoiceStart",ok}, {"sceVoiceStop",ok},
    {"sceVoiceCreatePort",voice_create_port}, {"sceVoiceDeletePort",ok},
    {"sceVoiceConnectIPortToOPort",ok}, {"sceVoiceDisconnectIPortFromOPort",ok},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",ok},
    {"_ZN3sce3Xml11InitializerC1Ev",xml_this}, {"_ZN3sce3Xml11InitializerD1Ev",xml_this},
    {"_ZN3sce3Xml6StringC1Ev",xml_this},
    {"_ZN3sce3Xml11Initializer10initializeEPKNS0_13InitParameterE",xml_initialize},
    {"sceGameCustomDataDialogInitialize",ok}, {"sceGameCustomDataDialogTerminate",ok},
    {"sceGameCustomDataDialogOpen",ok}, {"sceGameCustomDataDialogUpdateStatus",custom_dialog_status},
    {"sceNpTusCreateNpTitleCtxA",signed_out}, {"sceNpTssCreateNpTitleCtxA",signed_out},
    {"sceNpTusCreateRequest",community_no_id}, {"sceNpTusDeleteRequest",community_no_id},
    {"sceNpTusAbortRequest",community_no_id}, {"sceNpTusDeleteNpTitleCtx",community_no_id},

    /* The asynchronous forms name the same requests that cannot exist, and so does polling one. */
    {"sceNpTssGetDataAsync",community_no_id}, {"sceNpTusPollAsync",community_no_id},
    {"sceNpTusAddAndGetVariableAAsync",community_no_id}, {"sceNpTusAddAndGetVariableAVUserAsync",community_no_id},
    {"sceNpTusGetDataAAsync",community_no_id}, {"sceNpTusGetDataAVUserAsync",community_no_id},
    {"sceNpTusGetMultiSlotVariableAAsync",community_no_id}, {"sceNpTusGetMultiSlotVariableAVUserAsync",community_no_id},
    {"sceNpTusSetDataAAsync",community_no_id}, {"sceNpTusSetDataAVUserAsync",community_no_id},
    {"sceNpScoreCreateNpTitleCtxA",score_ctx_a},

    /* Asynchronous ranking and record calls fail as the synchronous ones; nothing is in flight to poll. */
    {"sceNpScoreGetBoardInfoAsync",signed_out}, {"sceNpScoreGetFriendsRankingAAsync",signed_out},
    {"sceNpScoreGetGameDataByAccountIdAsync",signed_out}, {"sceNpScoreGetRankingByAccountIdAsync",signed_out},
    {"sceNpScoreGetRankingByRangeAAsync",signed_out}, {"sceNpScoreRecordGameDataAsync",signed_out},
    {"sceNpScoreRecordScoreAsync",signed_out}, {"sceNpScorePollAsync",community_no_id},
    {"sceNpMatching2CreateContextA",matching2_context_a},
    {"sceNpMatching2CreateJoinRoomA",matching2_no_context}, {"sceNpMatching2JoinRoomA",matching2_no_context},

    {"sceNpMatching2GetRoomDataExternalList",matching2_no_context},

    {"sceNpMatching2GetRoomMemberDataExternalList",matching2_no_context},

    {"sceNpMatching2RegisterRoomMessageCallback",matching2_no_context},

    {"sceNpWebApiCreateContextA",webapi_context_a}, {"sceNpWebApiCreateHandle",webapi_handle},
    {"sceNpWebApiDeleteHandle",ok}, {"sceNpWebApiSendRequest2",signed_out},

    {"sceNpWebApiCreateExtdPushEventFilter",webapi_filter}, {"sceNpWebApiDeleteExtdPushEventFilter",ok},
    {"sceNpWebApiRegisterExtdPushEventCallbackA",webapi_callback_a},
    {"sceNpWebApiUnregisterExtdPushEventCallback",ok},
    {"sceNpWordFilterCreateTitleCtxA",signed_out}, {"sceNpWordFilterDeleteTitleCtx",community_no_id},
    {"sceNpWordFilterDeleteRequest",community_no_id},
    {"sceNpWordFilterAbortRequest",community_no_id},
    {"sceNpWordFilterCensorComment",community_no_id}, {"sceNpWordFilterCreateAsyncRequest",community_no_id},
    {"sceNpWordFilterPollAsync",community_no_id},

    {"sceNpInGameMessageInitialize",object}, {"sceNpInGameMessageTerminate",ok},
    {"sceNpInGameMessageCreateHandle",object}, {"sceNpInGameMessageDeleteHandle",ok},
    {"sceNpInGameMessageAbortHandle",ok},
    {"sceNpInGameMessagePrepareA",signed_out}, {"sceNpInGameMessageSendDataA",signed_out},

    {"sceNpAuthCreateRequest",auth_request}, {"sceNpAuthGetAuthorizationCodeA",auth_code_a},
    {"sceNpAuthAbortRequest",ok},
    {"sceNpAuthCreateAsyncRequest",auth_request}, {"sceNpAuthGetAuthorizationCode",auth_code_a},
    {"sceNpAuthPollAsync",auth_poll}, {"sceNpCmpOnlineId",cmp_online_id},
    {"sceNpTssCreateNpTitleCtx",signed_out}, {"sceNpScoreCreateNpTitleCtx",signed_out},
    {"sceNpScoreGetRankingByNpIdAsync",signed_out}, {"sceNpScoreGetRankingByRangeAsync",signed_out},
    {"sceNpScoreGetFriendsRankingAsync",signed_out}, {"sceNpScoreGetGameDataAsync",signed_out},
    {"sceNpMatching2CreateContext",signed_out},
    {"sceNpMatching2CreateJoinRoom",matching2_no_context}, {"sceNpMatching2JoinRoom",matching2_no_context},
    {"sceNpWebApiCreateContext",signed_out},
    {"sceNpWebApiCreatePushEventFilter",webapi_handle}, {"sceNpWebApiCreateServicePushEventFilter",webapi_handle},
    {"sceNpWebApiDeletePushEventFilter",ok}, {"sceNpWebApiDeleteServicePushEventFilter",ok},
    {"sceNpWebApiRegisterPushEventCallback",signed_out}, {"sceNpWebApiRegisterServicePushEventCallback",signed_out},
    {"sceNpWebApiUnregisterPushEventCallback",ok}, {"sceNpWebApiUnregisterServicePushEventCallback",ok},
    {"sceInvitationDialogOpen",invitation_open}, {"sceInvitationDialogGetResult",invitation_result},

    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpenA",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogGetResultA",invitation_result},
    {"sceInvitationDialogTerminate",invitation_term}, {"sceInvitationDialogClose",invitation_close},
    {"sceErrorDialogInitialize",error_init}, {"sceErrorDialogOpen",error_open},
    {"sceErrorDialogUpdateStatus",error_status}, {"sceErrorDialogTerminate",error_term},
    {"sceErrorDialogClose",error_close},
    {"sceNpFriendListDialogInitialize",friends_init}, {"sceNpFriendListDialogOpenA",friends_open},
    {"sceNpFriendListDialogUpdateStatus",friends_status}, {"sceNpFriendListDialogGetResultA",friends_result},
    {"sceNpFriendListDialogClose",friends_close}, {"sceNpFriendListDialogTerminate",friends_term},
};
uintptr_t runtime_np_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
