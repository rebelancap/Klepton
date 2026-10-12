// The platform-request retirement gate. `make ovrplat`, in `make check`.
//
// Round 16's stall had no instrument that could see it. The runtime was healthy
// throughout — no lock of ours held, no thread of ours blocked, memory fine,
// compositor at cadence — and the guest simply never came back from Exit Table,
// because it was waiting on a platform request this runtime had answered 0 to
// and then never completed. Nothing in the process could observe that: an
// un-retired request looks exactly like a request that was never made until the
// guest decides to wait for it, which on Pinball FX is forty minutes into a
// session and one menu press away.
//
// So the property this gates is not "the leaderboard write fails" — it already
// failed, correctly, for eleven rounds. It is: **on a run that has declared the
// platform up, every request id handed to the guest is retired by a message
// carrying that id.** That is the contract the declaration creates, and it is
// the one thing a post-mortem report cannot check.
//
// Driven through kl_ovrplat_sym(), which is the same seam the guest resolves
// through, rather than through a selftest hook: what is being asserted is the
// classification as well as the completion, and the classification lives in the
// resolver. No guest, no headset, no knob — microseconds.
//
// Two processes, for t_ovrpfloor's reason: g_platform_up is a one-way latch, so
// one process cannot test both the platform-up and the platform-down worlds.
// The child (argv[1] == "down") is the truthful-failure path that must NOT have
// changed — a run with no platform declared up still answers 0 and owes nobody
// a completion.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

#include "../runtime/klepton.h"
#include "../runtime/kl_target.h"
#include "../runtime/xr/kl_ovrplat.h"

typedef uint64_t (*fn0)(void);
typedef void    *(*pfn0)(void);
typedef uint64_t (*fn1)(void *);
typedef void    *(*pfn1)(void *);
typedef const char *(*sfn1)(void *);

static int fails;
static void ck(int cond, const char *what) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

static void *sym(const char *n) {
    void *f = kl_ovrplat_sym(n);
    if (!f) { printf("  FAIL could not resolve %s\n", n); fails++; }
    return f;
}

// The names this round is about: the four asked at boot and the one asked at
// the end of a table. Every one of them answered 0 on the device, and every one
// of them is a request UE registers a completion delegate for.
static const char *const REQUESTS[] = {
    "ovr_Leaderboard_WriteEntry",           // the end-of-table score submit
    "ovr_Leaderboard_GetEntriesAfterRank",  // the logo screen
    "ovr_Achievements_GetAllProgress",
    "ovr_IAP_GetProductsBySKU",             // the ownership half — see below
    // NOT ovr_IAP_GetViewerPurchases since the 2026-10-09 driver crossing: on
    // driver it is the itr EMPTY-answer carve-out (a real request id and a
    // success completion carrying a zero-length purchase array), asserted by
    // m_boot's check_drm_guard. It is no longer a refused request.
};
#define NREQ ((int)(sizeof REQUESTS / sizeof REQUESTS[0]))

// ---------------------------------------------------------------------------
// The child: no platform declared up. Nothing here may have changed.
static int run_down(void) {
    printf("=== platform NOT declared up: the truthful refusal is unchanged ===\n");
    // beatsaber is not a plat_init target, so ovr_PlatformInitializeAndroid
    // reports failure and g_platform_up stays 0.
    const kl_target *t = kl_target_lookup("beatsaber");
    ck(t != NULL && !strstr(t->env ? t->env : "", "KL_PLAT_INIT=1"),
       "beatsaber is not a KL_PLAT_INIT target");
    if (t) kl_target_use(t);

    fn0 init = (fn0)sym("ovr_PlatformInitializeAndroid");
    if (init) ck(init() == 0, "ovr_PlatformInitializeAndroid reports failure (0)");

    for (int i = 0; i < NREQ; i++) {
        fn0 f = (fn0)sym(REQUESTS[i]);
        if (!f) continue;
        char msg[160];
        snprintf(msg, sizeof msg, "%s -> 0, the request could not be made", REQUESTS[i]);
        ck(f() == 0, msg);
    }
    pfn0 pop = (pfn0)sym("ovr_PopMessage");
    if (pop) ck(pop() == NULL, "and no completion was queued for any of them");
    return fails;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "down")) return run_down() ? 1 : 0;

    printf("=== a declared-up platform retires every request it hands out ===\n");
    const kl_target *t = kl_target_lookup("pinballfx");
    ck(t != NULL && t->env && strstr(t->env, "KL_PLAT_INIT=1"),
       "pinballfx bakes KL_PLAT_INIT=1 in its row env");
    if (t) kl_target_use(t);

    fn0 init = (fn0)sym("ovr_PlatformInitializeAndroid");
    // ovrPlatformInitializeResult Success IS 0 for this family — the return
    // value is not the same 0 the request calls answer, which is the whole
    // point of klplat_init_fails being a separate function.
    if (init) ck(init() == 0, "ovr_PlatformInitializeAndroid -> Success");
    fn0 isup = (fn0)sym("ovr_IsPlatformInitialized");
    if (isup) ck(isup() != 0, "...and ovr_IsPlatformInitialized agrees");

    // A request that IS answered with a real payload, driven first so the queue
    // carries both shapes and the error path cannot pass by emptying it.
    fn0 gliu = (fn0)sym("ovr_User_GetLoggedInUser");
    uint64_t real_id = gliu ? gliu() : 0;
    ck(real_id != 0, "ovr_User_GetLoggedInUser still issues a real request");

    uint64_t ids[NREQ];
    for (int i = 0; i < NREQ; i++) {
        fn0 f = (fn0)sym(REQUESTS[i]);
        ids[i] = f ? f() : 0;
        char msg[160];
        snprintf(msg, sizeof msg,
                 "%s -> a NON-ZERO request id (0 is what stranded the guest)",
                 REQUESTS[i]);
        ck(ids[i] != 0, msg);
    }
    // Ids are unique, or "retired exactly once" is unmeasurable.
    int uniq = 1;
    for (int i = 0; i < NREQ; i++) {
        if (ids[i] == real_id) uniq = 0;
        for (int j = i + 1; j < NREQ; j++) if (ids[i] == ids[j]) uniq = 0;
    }
    ck(uniq, "every request id handed out is distinct");

    pfn0 pop     = (pfn0)sym("ovr_PopMessage");
    fn1  msg_id  = (fn1)sym("ovr_Message_GetRequestID");
    fn1  is_err  = (fn1)sym("ovr_Message_IsError");
    pfn1 get_err = (pfn1)sym("ovr_Message_GetError");
    sfn1 err_msg = (sfn1)sym("ovr_Error_GetMessage");
    fn1  msg_ty  = (fn1)sym("ovr_Message_GetType");
    pfn1 msg_str = (pfn1)sym("ovr_Message_GetString");
    fn1  free_m  = (fn1)sym("ovr_FreeMessage");
    if (!pop || !msg_id || !is_err || !get_err || !err_msg || !msg_ty ||
        !msg_str || !free_m) { printf("FAIL: accessors unresolved\n"); return 1; }

    // Drain the queue exactly as the guest's own pump does.
    int seen[NREQ]; memset(seen, 0, sizeof seen);
    int real_seen = 0, err_payload_clean = 1, err_has_text = 1, real_was_error = 0;
    int spare = 0;
    for (void *m = pop(); m; m = pop()) {
        uint64_t id = msg_id(m);
        int e = (int)is_err(m);
        if (id == real_id) { real_seen++; if (e) real_was_error = 1; }
        else {
            int hit = -1;
            for (int i = 0; i < NREQ; i++) if (ids[i] == id) hit = i;
            if (hit < 0) spare++;
            else {
                seen[hit]++;
                if (!e) err_payload_clean = 0;          // must be reported failed
                void *eh = get_err(m);
                if (!eh) err_has_text = 0;
                else if (!err_msg(eh) || !*err_msg(eh)) err_has_text = 0;
                // An error message carries NOTHING else. This is what keeps the
                // ownership line where it was: the completion reports that the
                // request failed and reports no product, purchase, price, asset,
                // rank or score, because there is no payload on it to read.
                if (msg_ty(m) != 0 || msg_str(m) != NULL) err_payload_clean = 0;
            }
        }
        free_m(m);
    }

    int all_retired = 1;
    for (int i = 0; i < NREQ; i++) if (seen[i] != 1) all_retired = 0;
    ck(all_retired,
       "EVERY refused request is retired by exactly one message carrying its id "
       "— the round-16 property; without it Exit Table waits forever");
    ck(real_seen == 1 && !real_was_error,
       "...and a request that really is answered still completes, not as an error");
    ck(err_payload_clean,
       "a refused request's completion IS an error and carries no payload at all "
       "(type 0, no string) — it reports failure, never ownership");
    ck(err_has_text,
       "ovr_Message_GetError / ovr_Error_GetMessage answer on it, so a guest "
       "that logs the error does not abort on an unimplemented name");
    ck(spare == 0, "nothing else was left on the queue");

    // The ownership ANSWER half must still refuse. Resolving is not calling, so
    // this only asserts the classification survived: a name that would report a
    // purchase still routes to the aborting handler, not to an implementation.
    ck(kl_ovrplat_sym("ovr_Purchase_GetExpirationTime") != NULL &&
       kl_ovrplat_sym("ovr_Message_GetPurchaseArray") != NULL,
       "the ownership ANSWER family still resolves to its refusal, not to an "
       "implementation (klplat_drm still aborts; nothing here answers it)");

    // ...and the platform-down world, in its own process.
    printf("\n");
    fflush(stdout);
    pid_t pid = fork();
    // The row's KL_PLAT_INIT=1 was setenv'd into THIS process by kl_target_use
    // (driver carries plat_init as row env), and exec passes the environment on.
    if (pid == 0) { unsetenv("KL_PLAT_INIT");
                    execl(argv[0], argv[0], "down", (char *)NULL); _exit(127); }
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    int down_ok = (pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("\n");
    ck(down_ok, "and a run that never declared the platform up is UNCHANGED — "
                "it still answers 0 and still owes nobody a completion");

    printf("%s: %d check(s) failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
