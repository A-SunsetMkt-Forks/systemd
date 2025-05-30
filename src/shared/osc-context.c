/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/auxv.h>

#include "escape.h"
#include "format-util.h"
#include "hostname-setup.h"
#include "id128-util.h"
#include "osc-context.h"
#include "pidfd-util.h"
#include "process-util.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "user-util.h"

/* This currently generates open sequences for OSC 3008 types "boot", "container", "vm", "elevate", "chpriv",
 * "subcontext", "session", "service".
 *
 * NB: Not generated by systemd: "remote" (would have to be generated from the SSH client), "app". */

static int strextend_escaped(char **s, const char *prefix, const char *value) {
        assert(s);
        assert(value);

        if (!strextend(s, prefix))
                return -ENOMEM;

        _cleanup_free_ char *e = xescape(value, ";\\");
        if (!e)
                return -ENOMEM;

        if (!strextend(s, e))
                return -ENOMEM;

        return 0;
}

static int osc_append_identity(char **s) {
        int r;

        assert(s);

        _cleanup_free_ char *u = getusername_malloc();
        if (u) {
                r = strextend_escaped(s, ";user=", u);
                if (r < 0)
                        return r;
        }

        _cleanup_free_ char *h = gethostname_malloc();
        if (h) {
                r = strextend_escaped(s, ";hostname=", h);
                if (r < 0)
                        return r;
        }

        sd_id128_t id;
        if (sd_id128_get_machine(&id) >= 0) {
                r = strextendf(s, ";machineid=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id));
                if (r < 0)
                        return r;
        }

        if (sd_id128_get_boot(&id) >= 0) {
                r = strextendf(s, ";bootid=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id));
                if (r < 0)
                        return r;
        }

        r = strextendf(s, ";pid=" PID_FMT, getpid_cached());
        if (r < 0)
                return r;

        uint64_t pidfdid;
        if (pidfd_get_inode_id_self_cached(&pidfdid) >= 0) {
                r = strextendf(s, ";pidfdid=%" PRIu64, pidfdid);
                if (r < 0)
                        return r;
        }

        r = strextend_escaped(s, ";comm=", program_invocation_short_name);
        if (r < 0)
                return r;

        return 0;
}

static void osc_context_default_id(sd_id128_t *ret_id) {

        /* Usually we only want one context ID per tool. Since we don't want to store the ID let's just hash
         * one from process credentials */

        struct {
                const char label[32];
                uint64_t pidfdid;
                uint8_t auxval[16];
                pid_t pid;
        } data = {
                .label = "systemd context hash bytes v1",
                .pid = getpid_cached(),
        };

        assert(ret_id);

        memcpy(data.auxval, ULONG_TO_PTR(getauxval(AT_RANDOM)), sizeof(data.auxval));
        (void) pidfd_get_inode_id_self_cached(&data.pidfdid);

        *ret_id = id128_digest(&data, sizeof(data));
}

static int osc_context_intro_raw(sd_id128_t id, char **ret_seq) {
        int r;

        assert(ret_seq);

        _cleanup_free_ char *seq = NULL;
        if (asprintf(&seq, ANSI_OSC "3008;start=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id)) < 0)
                return -ENOMEM;

        r = osc_append_identity(&seq);
        if (r < 0)
                return r;

        *ret_seq = TAKE_PTR(seq);
        return 0;
}

static int osc_context_intro(char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        assert(ret_seq);

        /* If the user passed us a buffer for the context ID generate a randomized one, since we have a place
         * to store it. The user should pass the ID back to osc_context_close() later on. If the user did not
         * pass us a buffer, we'll use a session ID hashed from process properties that remain stable as long
         * our process exists. It hence also remains stable across reexec and similar. */
        sd_id128_t id;
        if (ret_context_id) {
                r = sd_id128_randomize(&id);
                if (r < 0)
                        return r;
        } else
                osc_context_default_id(&id);

        r = osc_context_intro_raw(id, ret_seq);
        if (r < 0)
                return r;

        if (ret_context_id)
                *ret_context_id = id;

        return 0;
}

static int osc_context_outro(char *_seq, sd_id128_t id, char **ret_seq, sd_id128_t *ret_context_id) {
        _cleanup_free_ char *seq = TAKE_PTR(_seq); /* We take possession of the string no matter what */

        if (ret_seq)
                *ret_seq = TAKE_PTR(seq);
        else {
                fputs(seq, stdout);
                fflush(stdout);
        }

        if (ret_context_id)
                *ret_context_id = id;

        return 0;
}

int osc_context_open_boot(char **ret_seq) {
        int r;

        _cleanup_free_ char *seq = NULL;
        r = osc_context_intro(&seq, /* ret_context_id= */ NULL);
        if (r < 0)
                return r;

        if (!strextend(&seq, ";type=boot" ANSI_ST))
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), /* context_id= */ SD_ID128_NULL, ret_seq, /* ret_context_id= */ NULL);
}

int osc_context_open_container(const char *name, char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        _cleanup_free_ char *seq = NULL;
        sd_id128_t id = SD_ID128_NULL;
        r = osc_context_intro(&seq, ret_context_id ? &id : NULL);
        if (r < 0)
                return r;

        if (name) {
                r = strextend_escaped(&seq, ";container=", name);
                if (r < 0)
                        return r;
        }

        if (!strextend(&seq, ";type=container" ANSI_ST))
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, ret_context_id);
}

int osc_context_open_vm(const char *name, char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        assert(name);

        _cleanup_free_ char *seq = NULL;
        sd_id128_t id = SD_ID128_NULL;
        r = osc_context_intro(&seq, ret_context_id ? &id : NULL);
        if (r < 0)
                return r;

        r = strextend_escaped(&seq, ";vm=", name);
        if (r < 0)
                return r;

        if (!strextend(&seq, ";type=vm" ANSI_ST))
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, ret_context_id);
}

int osc_context_open_chpriv(const char *target_user, char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        assert(target_user);

        _cleanup_free_ char *seq = NULL;
        sd_id128_t id = SD_ID128_NULL;
        r = osc_context_intro(&seq, ret_context_id ? &id : NULL);
        if (r < 0)
                return r;

        if (is_this_me(target_user) > 0) {
                if (!strextend(&seq, ";type=subcontext" ANSI_ST))
                        return -ENOMEM;
        } else if (STR_IN_SET(target_user, "root", "0")) {
                if (!strextend(&seq, ";type=elevate" ANSI_ST))
                        return -ENOMEM;
        } else {
                r = strextend_escaped(&seq, ";targetuser=", target_user);
                if (r < 0)
                        return r;

                if (!strextend(&seq, ";type=chpriv" ANSI_ST))
                        return -ENOMEM;
        }

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, ret_context_id);
}

int osc_context_open_session(const char *user, const char *session_id, char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        _cleanup_free_ char *seq = NULL;
        sd_id128_t id = SD_ID128_NULL;
        r = osc_context_intro(&seq, ret_context_id ? &id : NULL);
        if (r < 0)
                return r;

        if (user) {
                r = strextend_escaped(&seq, ";targetuser=", user);
                if (r < 0)
                        return r;
        }

        if (session_id) {
                r = strextend_escaped(&seq, ";sessionid=", session_id);
                if (r < 0)
                        return r;
        }

        if (!strextend(&seq, ";type=session" ANSI_ST))
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, ret_context_id);
}

int osc_context_open_service(const char *unit, sd_id128_t invocation_id, char **ret_seq) {
        int r;

        sd_id128_t id = SD_ID128_NULL;
        r = osc_context_id_from_invocation_id(invocation_id, &id);
        if (r < 0)
                return r;

        _cleanup_free_ char *seq = NULL;
        r = osc_context_intro_raw(id, &seq);
        if (r < 0)
                return r;

        if (unit) {
                r = strextend_escaped(&seq, ";servicename=", unit);
                if (r < 0)
                        return r;
        }

        r = strextendf(&seq, ";invocationid=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(invocation_id));
        if (r < 0)
                return r;

        if (!strextend(&seq, ";type=service" ANSI_ST))
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, /* ret_context_id= */ NULL);
}

int osc_context_close(sd_id128_t id, char **ret_seq) {

        if (sd_id128_is_null(id)) {
                /* nil uuid: no session opened */
                if (ret_seq)
                        *ret_seq = NULL;
                return 0;
        }

        if (sd_id128_is_allf(id)) /* max uuid: default session opened */
                osc_context_default_id(&id);

        _cleanup_free_ char *seq = NULL;
        if (asprintf(&seq, ANSI_OSC "3008;end=" SD_ID128_FORMAT_STR ANSI_ST, SD_ID128_FORMAT_VAL(id)) < 0)
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), SD_ID128_NULL, ret_seq, /* ret_context_id= */ NULL);
}

int osc_context_id_from_invocation_id(sd_id128_t id, sd_id128_t *ret) {
        static const sd_id128_t app_id = SD_ID128_MAKE(5d,63,a5,8d,96,fd,45,d0,a0,f0,63,50,fc,d8,9a,cd);

        assert(ret);

        /* Hash the context id from the invocation id, so that we don't have to store the context id (as long
         * as we have the invocation ID). We don't use the invocation ID literally in order not to make it too
         * easy to cancel our service context because the invocation ID is reported literally via the
         * $INVOCATION_ID env var. */
        return sd_id128_get_app_specific(id, app_id, ret);
}
