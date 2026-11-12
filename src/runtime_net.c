/* Networking for an offline console (the cable is unplugged, as in runtime_services.c): the
 * libSceHttp calls runtime_services.c does not have create objects, and no request ever gets a
 * response (HTTP NETWORK); the Second Screen companion server never sees a client. */
#include "runtime.h"
#include <ctype.h>
#include <stdatomic.h>
#include <time.h>

/* ---- libSceHttp (shadPS4 http.cpp), offline ---- */
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_UNKNOWN_SCHEME ((int32_t)0x80431061)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
static _Atomic int32_t http_next=0x10000;
static int same(const char *a,const char *b) {
    while (*a && tolower((unsigned char)*a)==*b) ++a, ++b;
    return !*a && !*b;
}
/* sceHttpCreateConnection(template, server, scheme, port, keepalive): an id; the server is
 * required and the scheme is http or https. */
static ABI int32_t http_connection(int32_t tmpl,const char *server,const char *scheme,uint16_t port,int32_t keepalive) {
    (void)tmpl; (void)port; (void)keepalive;
    if (!server) return HTTP_INVALID_VALUE;
    if (!scheme || (!same(scheme,"http") && !same(scheme,"https"))) return HTTP_UNKNOWN_SCHEME;
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpCreateRequest2(connection, method, path, content length): an id; the path is required. */
static ABI int32_t http_request2(int32_t conn,const char *method,const char *path,uint64_t length) {
    (void)conn; (void)method; (void)length;
    if (!path) return HTTP_INVALID_VALUE;
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpGetAllResponseHeaders(request, header, size): no request gets a response. */
static ABI int32_t http_headers(int32_t req,char **header,uint64_t *size) {
    (void)req;
    if (!header || !size) return HTTP_INVALID_VALUE;
    return HTTP_NETWORK;
}
/* Timeouts and aborting a request: accepted, as shadPS4 does; nothing is sent. */
static ABI int32_t http_ok(void) { return 0; }
/* sceHttpCreateConnectionWithURL(tmpl, url, keepalive) / sceHttpCreateRequestWithURL(conn,
 * method, url, length): ids as their non-URL forms; nothing is sent (sceHttpSendRequest fails). */
static int32_t next_url_object=0x4000;
static ABI int32_t http_url_object(void) { return __atomic_fetch_add(&next_url_object,1,__ATOMIC_RELAXED); }
/* sceNetInit / sceNetTerm: 0. sceNetResolverStartNtoa(resolver, host, addr, timeout, retries,
 * flags): no network: RESOLVER ENODNS-style failure, errno ENETUNREACH. */
#define NET_RESOLVER_ENODNS ((int32_t)0x804101E1)
static ABI int32_t net_resolver_ntoa(void) { return NET_RESOLVER_ENODNS; }

/* libSceCompanionHttpd: the HTTP server for the PS4 Second Screen companion app. It starts and
 * accepts handlers, but no companion app can reach it, so no event ever arrives: GetEvent reports
 * none (the game only tests for nonzero; it reacts to 0x10000001 connected / 0x10000002 gone). */
#define COMPANION_NO_EVENT ((int32_t)0x80E40008)
static ABI int32_t companion_no_event(void *event) { (void)event; return COMPANION_NO_EVENT; }

/* libSceHttp's epoll (Dead Nation's non-blocking requests): a handle the requests attach to;
 * no request ever completes, so a wait sees no event (after its timeout, at most 16 ms, so a
 * poll loop does not spin). */
static ABI int32_t http_create_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return HTTP_INVALID_VALUE;
    *handle=(void *)(uintptr_t)atomic_fetch_add(&http_next,1); return 0;
}
static ABI int32_t http_wait_request(void *handle,void *events,int32_t max,int32_t timeout_us) {
    (void)handle; (void)events; (void)max;
    if (timeout_us!=0) {
        const int32_t us=timeout_us<0 || timeout_us>16000 ? 16000 : timeout_us;
        struct timespec ts={0,(long)us*1000};
        nanosleep(&ts,NULL);
    }
    return 0;
}

/* POSIX sockets (libScePosix): no network interface is up. socket() fails with ENETDOWN; a call
 * on a descriptor that therefore cannot exist fails with EBADF. */
static ABI int32_t sock_netdown(void) { *runtime_errno()=50; return -1; }
static ABI int32_t sock_badf(void) { *runtime_errno()=9; return -1; }

static const RuntimeExport exports[]={
    {"sceCompanionHttpdInitialize",http_ok}, {"sceCompanionHttpdOptParamInitialize",http_ok},
    {"sceHttpCreateEpoll",http_create_epoll}, {"sceHttpDestroyEpoll",http_ok},
    {"sceHttpSetEpoll",http_ok}, {"sceHttpUnsetEpoll",http_ok}, {"sceHttpSetNonblock",http_ok},
    {"sceHttpWaitRequest",http_wait_request}, {"sceNetShowIfconfig",http_ok},
    {"socket",sock_netdown}, {"bind",sock_badf}, {"connect",sock_badf}, {"accept",sock_badf},
    {"setsockopt",sock_badf}, {"select",sock_badf}, {"recv",sock_badf}, {"send",sock_badf},
    {"recvfrom",sock_badf}, {"sendto",sock_badf}, {"shutdown",sock_badf},
    {"sceCompanionHttpdInitialize2",http_ok}, {"sceCompanionHttpdTerminate",http_ok},
    {"sceCompanionHttpdStart",http_ok}, {"sceCompanionHttpdStop",http_ok},
    {"sceCompanionHttpdRegisterRequestCallback",http_ok}, {"sceCompanionHttpdUnregisterRequestCallback",http_ok},
    {"sceCompanionHttpdAddHeader",http_ok}, {"sceCompanionHttpdSetBody",http_ok},
    {"sceCompanionHttpdSetStatus",http_ok}, {"sceCompanionHttpdGetEvent",companion_no_event},

    {"sceHttpCreateConnection",http_connection}, {"sceHttpCreateRequest2",http_request2},
    {"sceHttpGetAllResponseHeaders",http_headers},
    {"sceHttpAbortRequest",http_ok}, {"sceHttpsSetSslCallback",http_ok},
    {"sceHttpCreateConnectionWithURL",http_url_object}, {"sceHttpCreateRequestWithURL",http_url_object},
    {"sceNetInit",http_ok}, {"sceNetTerm",http_ok}, {"sceNetResolverStartNtoa",net_resolver_ntoa},
    {"sceHttpSetSendTimeOut",http_ok}, {"sceHttpSetResolveTimeOut",http_ok},
    {"sceHttpSetRecvTimeOut",http_ok},
};
uintptr_t runtime_net_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
