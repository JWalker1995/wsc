/*
 * lws-minimal-ws-client-binance
 *
 * Written in 2010-2020 by Andy Green <andy@warmcat.com>
 *                         Kutoga <kutoga@user.github.invalid>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 * This demonstrates a ws client that connects to binance ws server efficiently
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <stdio.h>

const char *argUrl;
lws_sorted_usec_list_t sul;
uint16_t retryCount;
static struct lws_context *lwsContext;
static int interrupted;

/*
 * The retry and backoff policy we want to use for our client connections
 */

static const uint32_t backoff_ms[] = { 1000, 2000, 3000, 4000, 5000 };

static const lws_retry_bo_t retryConfig = {
    .retry_ms_table			= backoff_ms,
    .retry_ms_table_count		= LWS_ARRAY_SIZE(backoff_ms),
    .conceal_count			= LWS_ARRAY_SIZE(backoff_ms),

    .secs_since_valid_ping		= 400,  /* force PINGs after secs idle */
    .secs_since_valid_hangup	= 400, /* hangup after secs idle */

    .jitter_percent			= 0,
};

/*
 * If we don't enable permessage-deflate ws extension, during times when there
 * are many ws messages per second the server coalesces them inside a smaller
 * number of larger ssl records, for >100 mps typically >2048 records.
 *
 * This is a problem, because the coalesced record cannot be send nor decrypted
 * until the last part of the record is received, meaning additional latency
 * for the earlier members of the coalesced record that have just been sitting
 * there waiting for the last one to go out and be decrypted.
 *
 * permessage-deflate reduces the data size before the tls layer, for >100mps
 * reducing the colesced records to ~1.2KB.
 */

static const struct lws_extension extensions[] = {
    {
        "permessage-deflate",
        lws_extension_callback_pm_deflate,
        "permessage-deflate"
         "; client_no_context_takeover"
         "; client_max_window_bits"
    },
    { NULL, NULL, NULL /* terminator */ }
};
/*
 * Scheduled sul callback that starts the connection attempt
 */

static void connect_client(lws_sorted_usec_list_t *sul) {
    struct lws_client_connect_info i;

    memset(&i, 0, sizeof(i));

    char *url = new char[strlen(argUrl) + 2];
    strcpy(url, argUrl);

    const char *prot;
    if (lws_parse_uri(url, &prot, &i.address, &i.port, &i.path)) {
        lwsl_err("Cannot parse uri");
        exit(1);
    }

    char *path = const_cast<char *>(i.path);
    char p = *path;
    *path++ = '/';
    while (p) {
        std::swap(*path++, p);
    }
    *path = '\0';

    i.context = lwsContext;
    i.host = i.address;
    i.origin = i.address;
    i.ssl_connection = LCCSCF_USE_SSL | LCCSCF_PRIORITIZE_READS;
    i.protocol = NULL;
    i.local_protocol_name = "lws-minimal-client";
    i.retry_and_idle_policy = &retryConfig;

    if (!lws_client_connect_via_info(&i)) {
        /*
         * Failed... schedule a retry... we can't use the _retry_wsi()
         * convenience wrapper api here because no valid wsi at this
         * point.
         */
        if (lws_retry_sul_schedule(lwsContext, 0, sul, &retryConfig, connect_client, &retryCount)) {
            lwsl_err("%s: connection attempts exhausted\n", __func__);
            interrupted = 1;
        }
    }
}

static int callback_minimal(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        lwsl_err("LWS_CALLBACK_CLIENT_CONNECTION_ERROR: %s\n", in ? (char *)in : "(null)");
        goto do_retry;
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        puts(static_cast<const char *>(in));
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        lwsl_err("LWS_CALLBACK_CLIENT_CLOSED\n");
        goto do_retry;
        break;

    default:
        break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);

do_retry:
    /*
     * retry the connection to keep it nailed up
     *
     * For this example, we try to conceal any problem for one set of
     * backoff retries and then exit the app.
     *
     * If you set retry.conceal_count to be larger than the number of
     * elements in the backoff table, it will never give up and keep
     * retrying at the last backoff delay plus the random jitter amount.
     */
    if (lws_retry_sul_schedule_retry_wsi(wsi, &sul, connect_client, &retryCount)) {
        lwsl_err("%s: connection attempts exhausted\n", __func__);
        interrupted = 1;
    }

    return 0;
}

static const struct lws_protocols protocols[] = {
    { "lws-minimal-client", callback_minimal, 0, 0, },
    { NULL, NULL, 0, 0 }
};

static void sigint_handler(int sig) {
    interrupted = 1;
}

int main(int argc, const char **argv) {
    if (argc != 2) {
        lwsl_err("Usage: wsc [uri]");
        return 1;
    }

    argUrl = argv[1];

    struct lws_context_creation_info info;
    int n = 0;

    signal(SIGINT, sigint_handler);
    memset(&info, 0, sizeof info);
    lws_cmdline_option_handle_builtin(argc, argv, &info);

    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.port = CONTEXT_PORT_NO_LISTEN; /* we do not run any server */
    info.protocols = protocols;
    info.fd_limit_per_thread = 1 + 1 + 1;
    info.extensions = extensions;

    lwsContext = lws_create_context(&info);
    if (!lwsContext) {
        lwsl_err("lws init failed\n");
        return 1;
    }

    /* schedule the first client connection attempt to happen immediately */
    lws_sul_schedule(lwsContext, 0, &sul, connect_client, 1);

    while (n >= 0 && !interrupted) {
        n = lws_service(lwsContext, 0);
    }

    lws_context_destroy(lwsContext);

    return 0;
}

