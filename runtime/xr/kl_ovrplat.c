// libovrplatformloader.so — see kl_ovrplat.h for why this is a replacement and
// where the DRM line is drawn.
//
// ovr_IAP_GetViewerPurchases is ANSWERED with an EMPTY purchase array — the
// viewer owns nothing, which is the truthful state on a host with no Oculus
// Store account. It grants no access, unlocks no content and names no SKU; see
// the carve-out comment below.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>              // the cloud-save directory is created
#include "klepton.h"
#include "kl_jni.h"
#include "guest/kl_driver.h"   // target name, for the per-target KL_PLAT_USER default
#include "kl_env.h"
#include "kl_jni.h"                        // kl_jni_files_dir, for the save directory
#include "kl_target.h"                     // the per-target platform-gate answer
#include "kl_ovrplat.h"

#define KL_OVRPLAT_MAX 512
static struct { const char *name; unsigned calls; int drm; } g_plat[KL_OVRPLAT_MAX];
static unsigned g_nplat;
static unsigned g_drm_calls;

static int plat_slot(const char *name) {
    for (unsigned i = 0; i < g_nplat; i++)
        if (strcmp(g_plat[i].name, name) == 0) return (int)i;
    if (g_nplat >= KL_OVRPLAT_MAX) return -1;
    g_plat[g_nplat].name  = strdup(name);
    g_plat[g_nplat].calls = 0;
    g_plat[g_nplat].drm   = 0;
    return (int)g_nplat++;
}

static void plat_hit(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) g_plat[s].calls++;
}

// ---------------------------------------------------------------------------
// The DRM classifier
//
// Matched on substrings of the entry point name rather than an enumerated list,
// deliberately: the export surface is 1335 wide and a list would be the thing
// that goes stale. A new ovr_Entitlement_* or ovr_IAP_* added by a future SDK
// lands on the refusal by default, which is the direction an error should fall.
//
// What is covered and why:
//   Entitle*  — "is this user allowed to run this", the licence check. Note the
//               carve-out below: the APP's own entitlement is answered,
//               everything else in the family still refuses.
//   IAP       — purchases and their ownership records. Note the carve-out
//               below: the request-shaped calls answer 0, "the request could
//               not be made"; every accessor over a purchase still refuses.
//   AssetFile — download and unlock of purchasable DLC; the delivery half of the
//               same question. ovr_AssetFile_* is how paid content arrives.
//               Note the carve-outs below: the pure *enumeration* call is
//               answered — "nothing here" — the per-file Status calls are
//               answered as enumerations too, and Delete joins the request
//               carve-out. Download and DownloadCancel stay fatal; they are
//               the delivery itself.
//   Purchase  — belt and braces for the SDK's other spelling of IAP.
static const char *const g_drm_markers[] = {
    "Entitle", "Entitlement", "IAP", "Purchase", "AssetFile", "AssetDetails",
};

// The carve-outs: names in the ownership families that we ANSWER rather than
// refuse. Exact names, not substrings, and checked BEFORE the markers below, so
// the marker list keeps its fail-closed default and anything new still lands on
// the refusal.
//
// Two of them are upstream's:
//
//   ovr_Entitlement_GetIsViewerEntitled — the APPLICATION's own licence check,
//     and the one question in this family that can actually be answered: the
//     user unpacked an APK from a device they own, i.e. from a copy of the app
//     they already bought; answering yes describes that, it does not
//     manufacture it. Upstream keeps DLC ownership refused — paid content is
//     out-of-band asset data plus a licence held by a platform absent here.
//
//   ovr_AssetFile_GetList — enumerates the DLC asset files present on this
//     device; answered "nothing here", which grants no access.
//
// A THIRD is answered here, and it is a NEGATIVE answer rather than a
// permissive one:
//
//   ovr_IAP_GetViewerPurchases — "what has this viewer bought?" Answered with a
//     ZERO-LENGTH purchase array: nothing. That is the truthful state of a host
//     with no Oculus Store account and no purchase history, it is the same
//     shape as upstream's GetList carve-out ("nothing here"), and it grants
//     exactly nothing — no SKU is named, no entitlement is reported, no content
//     is unlocked. Into the Radius asks it on its way to the first frame (its
//     Supporter Pack check) and the refusal aborted the run before any frame
//     could land. Answering "you own nothing"
//     lets the game take its own no-DLC path — or gate itself, which is the
//     honest outcome and the answer to whether this target is viable at all.
//     The accessors over the array come with it, and they can only ever
//     describe an empty array: GetSize is 0, GetElement is NULL, HasNextPage
//     false, GetNextUrl NULL.
//     **ovr_Purchase_GetSKU is deliberately NOT carved out** — it reads a field
//     off a purchase, and there are no purchases. It is unreachable from an
//     empty array, so leaving it on the refusal costs nothing and keeps the
//     fail-closed default exactly where it should be: the moment anything here
//     ever produced a purchase to describe, the run would stop and say so.
//     Every IAP call that could DELIVER or BUY (ovr_IAP_LaunchCheckoutFlow,
//     _ConsumePurchase, _GetProductsBySKU, the next-page fetches) still aborts
//     through the "IAP" marker.
//
// A FOURTH, negative too: the per-file status spellings of the GetList
// question (ovr_AssetFile_Status / _StatusById / _StatusByName). Some titles
// never enumerate — Arkham Shadow asks about its language packs by name, one
// at a time — and a refusal there stops the game at its first frame. They are
// answered with a single AssetDetails saying "available" / "not-entitled" and
// no path, WHATEVER the name and whatever is on disk: the platform has it in
// its catalogue, this device does not have it, this user has no claim on it.
// Nothing is read from the filesystem, so nothing can be asserted from it.
//
// Everything that could DELIVER content (ovr_AssetFile_Download*,
// _DownloadCancel*, _Delete*) and every ownership / purchase query beyond the
// app's own stays refused.
static const char *const g_plat_enumeration[] = {
    "ovr_AssetFile_GetList",
    "ovr_AssetFile_Status",
    "ovr_AssetFile_StatusById",
    "ovr_AssetFile_StatusByName",
    "ovr_Message_GetAssetDetails",
    "ovr_AssetDetails_GetAssetId",
    "ovr_AssetDetails_GetAssetType",
    "ovr_AssetDetails_GetAssetType_Native",
    "ovr_AssetDetails_GetDownloadStatus",
    "ovr_AssetDetails_GetDownloadStatus_Native",
    "ovr_AssetDetails_GetFilepath",
    "ovr_AssetDetails_GetFilepath_Native",
    "ovr_AssetDetails_GetIapStatus",
    "ovr_AssetDetails_GetIapStatus_Native",
    "ovr_AssetDetails_GetLanguage",
    "ovr_AssetDetails_GetMetadata",
    "ovr_AssetDetails_GetMetadata_Native",

    // The EMPTY purchase answer and the accessors over it. See the
    // carve-out comment above: the array is always length 0, so none of these
    // can describe a purchase, and ovr_Purchase_* stays refused.
    "ovr_IAP_GetViewerPurchases",
    "ovr_Message_GetPurchaseArray",
    "ovr_PurchaseArray_GetSize",
    "ovr_PurchaseArray_GetElement",
    "ovr_PurchaseArray_GetNextUrl",
    "ovr_PurchaseArray_HasNextPage",
};

// The app-level licence check: answered yes, upstream's move of the line
// (e8089c7), which this branch inherits unchanged.
static const char *const g_plat_app_entitled[] = {
    "ovr_Entitlement_GetIsViewerEntitled",
    // The GetList completion's readers. They match the "AssetDetails"/"AssetFile"
    // DRM markers by substring, but they only READ the empty list GetList
    // returned - no id, no size, no delivery - so they carve out for the same
    // reason GetList does: strictly more restrictive than the truth. The
    // DELIVERY calls (ovr_AssetFile_Download*, DownloadById) are not here and
    // stay refused.
    "ovr_Message_GetAssetFileDeliveryList",
    "ovr_AssetFileDeliveryList_GetSize",
    "ovr_Message_GetAssetDetailsArray",
    "ovr_AssetDetailsArray_GetSize",
};

static int plat_is_enumeration(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_enumeration / sizeof g_plat_enumeration[0]; i++)
        if (strcmp(name, g_plat_enumeration[i]) == 0) return 1;
    return 0;
}

static int plat_is_app_entitled(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_app_entitled / sizeof g_plat_app_entitled[0]; i++)
        if (strcmp(name, g_plat_app_entitled[i]) == 0) return 1;
    return 0;
}

// The IAP CATALOG calls — enumeration of what is for sale, not what is owned.
// ovr_IAP_GetProductsBySKU returns a ProductArray of {sku, name, description,
// formatted price}: the store's shop window, read to DECIDE whether to offer a
// purchase. It reports no ownership and delivers no content, and on a host with
// no store the truthful answer is "the request could not be made" (request 0),
// the same answer ovr_User_GetLoggedInUser gives. This is the IAP twin of the
// ovr_AssetFile_GetList carve-out: a pure enumeration is strictly MORE
// restrictive than the truth. The OWNERSHIP half of the family is untouched and
// still refuses — ovr_IAP_GetViewerPurchases (what the user bought),
// ovr_IAP_ConsumePurchase, ovr_IAP_LaunchCheckoutFlow and every delivery call
// keep matching the "IAP"/"Purchase" markers below, because this list is exact
// names checked first, not a substring. GetNextProductArrayPage walks the same
// catalog array, so it carves out for the same reason.
static int plat_is_iap_catalog(const char *name) {
    return strcmp(name, "ovr_IAP_GetProductsBySKU") == 0 ||
           strcmp(name, "ovr_IAP_GetNextProductArrayPage") == 0;
}

// The read side of the OWNED half: "which IAPs has this viewer already bought?"
// This looks like an ownership query, and it is — but answering it truthfully on
// a host with no Oculus platform is "the request could not be made" (request id
// 0), exactly what ovr_User_GetLoggedInUser and the IAP catalog return. That is
// the OPPOSITE of circumvention: the guest is handed no purchase message, so it
// learns of no entitlement it did not already have. Fabrication would be
// DELIVERING a PurchaseArray that names something as owned, or answering the
// WRITE side — ovr_IAP_ConsumePurchase / ovr_IAP_LaunchCheckoutFlow and the
// AssetFile delivery calls — all of which stay in plat_is_drm and keep aborting.
// So the honest failure is carved out and the grant-shaped calls are not.
static int plat_is_iap_owned_query(const char *name) {
    return strcmp(name, "ovr_IAP_GetViewerPurchases") == 0 ||
           strcmp(name, "ovr_IAP_GetViewerPurchasesDeltaAsync") == 0;
}

// ---------------------------------------------------------------------------
// The ownership families' REQUEST half: the requests fail honestly, the
// accessors still refuse.
//
// Decided 2026-08-20, on the Pinball FX VR crash. Pinball FX is
// free-to-play and fetches its store catalogue during online init; the guest
// called ovr_IAP_GetProductsBySKU and klplat_drm() aborted, on every launch,
// at the splash. The line that moves is narrow and it is stated here so it can
// be read rather than inferred:
//
//   A call under this list RETURNS AN ovrRequest and completes through a
//   message. It is not an answer — it is a question asked of a service. On a
//   host with no Oculus platform the request genuinely CANNOT BE MADE, and 0
//   is how that is spelled (see g_plat_request below, which has drawn the same
//   distinction for RichPresence and the init calls all along). It reports no
//   product, no purchase, no price, no asset and no ownership. It grants
//   access to nothing.
//
//   NOTHING ELSE MOVES. Every accessor that would ASSERT or DELIVER ownership
//   keeps the unconditional abort: ovr_Purchase_*, ovr_Product_*,
//   ovr_AssetDetails_* beyond the enumeration carve-out above, and every
//   AssetFile Download / DownloadCancel spelling. Those are where a fabricated
//   0 would read as an ANSWER ("you own this", "this asset is installed"), and
//   that is the circumvention klplat_drm's paragraph is about. Keeping them
//   fatal costs nothing: a request that answered 0 queues no message, so no
//   accessor in these families is ever reachable from here.
//
// Exact names, taken from the export table of the guest's own
// libovrplatformloader.so — not a prefix — so the marker list keeps its
// fail-closed default and a future SDK's new ovr_IAP_* still lands on the
// refusal. The IAP family is listed whole because every one of its eight
// exports is request-shaped, including the two array-page walkers (which can
// only be reached with an array we never produce). AssetFile is listed by
// name: Status and Delete ask a service a question, Download and
// DownloadCancel are the delivery half and are deliberately absent.
//
// KL_PLAT_DRM_ABORT=1 restores the abort for these names, as the A/B.
static const char *const g_plat_drm_request[] = {
    "ovr_IAP_ConsumePurchase",
    "ovr_IAP_GetNextProductArrayPage",
    "ovr_IAP_GetNextPurchaseArrayPage",
    "ovr_IAP_GetProductsBySKU",
    "ovr_IAP_GetViewerPurchases",
    "ovr_IAP_GetViewerPurchasesDurableCache",
    // NOT ovr_IAP_LaunchCheckoutFlow: buying stays on the abort (driver's
    // guard asserts it), and a game whose catalogue is answered never needs it.
    "ovr_AssetFile_Delete",
    "ovr_AssetFile_DeleteById",
    "ovr_AssetFile_DeleteByName",
    "ovr_AssetFile_Status",
    "ovr_AssetFile_StatusById",
    "ovr_AssetFile_StatusByName",
};

static int plat_is_drm_request(const char *name) {
    if (kl_env_on("KL_PLAT_DRM_ABORT", 0)) return 0;   // the A/B: refuse as before
    // An enumeration carve-out (AssetFile_Status*, the EMPTY
    // ovr_IAP_GetViewerPurchases) has a real answer below; it is not a
    // request-that-could-not-be-made.
    if (plat_is_enumeration(name)) return 0;
    for (size_t i = 0; i < sizeof g_plat_drm_request / sizeof g_plat_drm_request[0]; i++)
        if (strcmp(name, g_plat_drm_request[i]) == 0) return 1;
    return 0;
}

static int plat_is_drm(const char *name) {
    if (plat_is_enumeration(name)) return 0;
    if (plat_is_app_entitled(name)) return 0;
    if (plat_is_iap_catalog(name)) return 0;
    if (plat_is_iap_owned_query(name)) return 0;
    if (plat_is_drm_request(name)) return 0;
    for (size_t i = 0; i < sizeof g_drm_markers / sizeof g_drm_markers[0]; i++)
        if (strstr(name, g_drm_markers[i])) return 1;
    return 0;
}

// Ownership and licence ANSWERS — everything the carve-outs above do NOT name:
// purchase records, asset details, product descriptions, and every call that
// would deliver content. This aborts unconditionally — KL_PERMISSIVE is a
// scouting knob for things we intend to implement, and this is not one of them.
// Answering 0 would be worse than useless here: for a call shaped like
// ovr_Purchase_GetExpirationTime or ovr_AssetFile_DownloadById a fabricated
// answer is the circumvention itself, and a permissive run would produce it
// silently.
//
// The request half of the same families is a different question and has a
// different answer — see klplat_drm_request below. Asking a question of an
// absent service and being told "the request could not be made" grants nothing;
// being handed a purchase record does.
static uint64_t klplat_drm(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) { g_plat[s].calls++; g_plat[s].drm = 1; }
    g_drm_calls++;
    fprintf(stderr,
            "\n[klepton] REFUSED: the guest called '%s'.\n"
            "  This is an entitlement / purchase-ownership query. Klepton does not\n"
            "  implement these and does not answer them permissively — inventing a\n"
            "  result is DRM circumvention, which is out of scope.\n"
            "  The Oculus platform is genuinely absent on this host; that is the\n"
            "  true state of affairs, and it is the app's decision what to do with\n"
            "  it. If this call is unavoidable, the target is wrong: pick an APK\n"
            "  without entitlement checks (Steam Link).\n", name);
    kl_ovrplat_report(stderr);
    kl_fatal_prepare();
    abort();
}

// Set by ovr_PlatformInitializeAndroid on a plat_init target: "this run has
// told the guest the platform came up". Declared here rather than beside the
// message queue because the refusal paths below are its first readers.
static int g_platform_up;

// Issue a request that FAILED, and queue the error completion that retires it.
// Defined below, next to the other request issuers; declared here because the
// two refusal paths above it are the ones that need it. See the long comment on
// its definition for why an unretired request is a process-lifetime hang.
static uint64_t klplat_request_error(const char *name, const char *why);
static int klplat_init_target(void);   // KL_PLAT_INIT, below

// ...and the request half of those same families, which does NOT abort: it
// reports FAILURE, because there is no Oculus service here to make the request
// of. See g_plat_drm_request above for the full statement of the line and for
// the 2026-08-20 decision that moved it.
//
// This is deliberately NOT klplat_init_fails: on a plat_init target that
// function reports the PLATFORM up and returns ovrPlatformInitializeResult
// Success, which is a different claim entirely. Here the platform's state is
// not in question — the request's is.
//
// **The policy line is unchanged by the round-16 error completion.** An error
// asserts nothing: it reports no product, no purchase, no price, no asset and
// no ownership, and every payload accessor in these families
// (ovr_Message_GetProductArray, ovr_Message_GetPurchaseArray, ovr_AssetFile_*)
// is still absent from both tables, so it is still unreachable and still fatal
// by name. What changes is only that the guest is TOLD its request finished.
static uint64_t klplat_drm_request(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) g_plat[s].calls++;
    int first = (s < 0 || g_plat[s].calls == 1);
    // Gated on KL_PLAT_INIT (the Unreal plat_init targets), NOT on
    // g_platform_up alone: a Unity guest also brings the platform up
    // (ovr_UnityInitWrapper / the async init), and for Unity a type-0 queued
    // error stalls the SDK's message pump — see "Why this branch does NOT
    // queue an ERROR message" below. Unity keeps driver's bare 0.
    if (g_platform_up && klplat_init_target())
        return klplat_request_error(name, first
            ? "no Oculus service on this host. No product, purchase, price, "
              "asset or ownership is reported; the accessors still refuse"
            : NULL);
    if (first)
        fprintf(stderr, "  [plat] %s -> 0 (request NOT made, and the platform has "
                        "not been declared up: no Oculus service on this host. No "
                        "product, purchase, price, asset or ownership is reported; "
                        "the accessors still refuse)\n", name);
    return 0;
}

// Everything else: named, answered 0, and COUNTED — the terminal tier.
//
// This used to abort, and the abort was costing a launch per name. Paint the
// Town Red stopped on ovr_Leaderboard_GetEntries after a boot that had already
// composited its picture, and the surface behind it is 700 names wide in this
// guest's own metadata: the SDK's whole C binding is compiled into libil2cpp.so
// whether the title calls it or not, so "the next unimplemented name" is a well
// that does not run dry one launch at a time.
//
// Zero is the honest answer for the overwhelming majority of what lands here,
// and it is the SAME answer the classified tiers above give: an ovrRequest of 0
// is "the request could not be made" (and on a Unity guest the SDK completes it
// client-side with a synthesized error — measured, see the note above
// g_plat_request_prefix), an ovrID of 0 is "no such user", a size of 0 is an
// empty array, and NULL is "no string / no sub-object". The classified tiers
// are still worth having: they say WHICH of those a name is, in the log and in
// the census, and this one cannot.
//
// What is deliberately NOT softened: plat_is_drm runs FIRST in kl_ovrplat_sym
// and still aborts unconditionally. The ownership line is a policy, not a work
// list, and nothing here is a way around it.
//
// The cost is that a name whose contract needs a non-NULL handle gets a NULL.
// Every such name in this guest's surface is classified above — the options
// cookie, the launch-details cookie, and the three device creators — so the
// residue is getters and commands. A run that answers any of these says so
// loudly, once per name, and kl_ovrplat_report counts them, which is what makes
// the next round's log a work list instead of a single tombstone.
static unsigned g_plat_unclassified;

static uint64_t klplat_called(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1) {
        g_plat_unclassified++;
        fprintf(stderr, "  [plat] %s -> 0 (UNCLASSIFIED Oculus Platform entry "
                        "point: no service here, and this name is not yet sorted "
                        "into a request / accessor / command tier. Zero is a "
                        "request that could not be made, an empty array and a "
                        "NULL string all at once; if this guest wanted something "
                        "else, this line is where to start)\n", name);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The completion queue — an asynchronous request has to ARRIVE somewhere
//
// An async request gets a real id and a message that arrives via
// ovr_Message_Get*. Answering `ovrRequest` 0 has the SAME "poll forever" tail
// as a fabricated id — the SDK builds its `Request` object either way and
// awaits it — so 0 buys silence, not an error path.
//
// Beat Saber 1.40 gates a screen on that silence: HealthWarningFlowCoordinator
// hands the epilepsy screen's Continue to WaitForUserAgeCategory, a coroutine
// waiting on a category OculusInit.CheckUserAgeCategoryAsync never sets because
// InitializeOculusAsync is still awaiting platform init. The button does
// nothing, silently, with the frame loop running normally.
//
// What is asserted, and by whose decision. Answering the age category at all
// claims an initialized platform, which is a real change from "genuinely
// absent" — an absent platform cannot answer a demographic question. The user
// asked for it and declared themselves over 18 (2026-08-12), which is the only
// place that answer can legitimately come from here: a self-declaration by the
// person running their own copy, not a discovered fact. THE DRM LINE DOES NOT
// MOVE WITH IT — IAP, purchase records, asset details and every delivery call
// still abort unconditionally, and check_drm_guard still asserts both
// directions. What changes is that the application's own entitlement, already
// answered yes, is answered through the channel the guest actually reads.
//
// The three completions are all this queues, each issued only in response to
// the guest's own request:
//
//   ovr_UnityInitWrapperAsynchronous  -> Platform_InitializeAndroidAsynchronous,
//                                        result Success
//   ovr_Entitlement_GetIsViewerEntitled -> Entitlement_GetIsViewerEntitled, no
//                                        payload; success IS the answer
//   ovr_UserAgeCategory_Get           -> UserAgeCategory_Get, category Ad(ult)
//
// The type numbers are the guest's own, read out of the running IL2CPP runtime
// with KL_PROBE_ENUM (kl_mprobe.c) rather than from an SDK header. Necessary
// rather than tidy: Message.ParseMessageHandle switches on the type and its
// default arm logs "Unrecognized message type" and produces NO message, so a
// guessed number is indistinguishable from the silence above.
#define KLPLAT_MSG_PLATFORM_INIT   450037684u   // Message.MessageType
#define KLPLAT_MSG_ENTITLEMENT     409688241u   //   .Platform_InitializeAndroidAsynchronous
#define KLPLAT_MSG_AGE_CATEGORY    567009472u   //   .Entitlement_GetIsViewerEntitled
#define KLPLAT_MSG_APP_VERSION    1751583246u   //   .UserAgeCategory_Get
                                                //   .Application_GetVersion
#define KLPLAT_MSG_CLOUD_DIR      1990471406u   //   .CloudStorage2_GetUserDirectoryPath
#define KLPLAT_MSG_ASSET_LIST     1258057588u   //   .AssetFile_GetList (KL_PROBE_ENUM, this APK)
#define KLPLAT_MSG_ACCESS_TOKEN     0x06A85ABEu  // .User_GetAccessToken (Message<string>)
#define KLPLAT_MSG_ACHIEVEMENT_DEFS 0x03D3458Du  // .Achievements_GetAllDefinitions (empty array)
#define KLPLAT_MSG_LOGGED_IN_USER  0x436F345Du  // .User_GetLoggedInUser (Message<User>)
// The same two completions as the KL_PLAT_INIT declared-user surface issues
// them (see klplat_init_User_GetLoggedInUser): the type numbers that surface
// was verified with. Unreal dispatches on the request id, not the type.
#define KLPLAT_MSG_INIT_LOGGED_IN_USER 0x1D81AC88u  // .User_GetLoggedInUser
#define KLPLAT_MSG_INIT_ACCESS_TOKEN   0x06B85AB8u  // .User_GetAccessToken
#define KLPLAT_INIT_SUCCESS        0            // PlatformInitializeResult.Success
#define KLPLAT_AGE_ADULT           3            // AccountAgeCategory.Ad

// The declared local user, served to targets that bake KL_PLAT_INIT=1 — and a
// DECLARATION in exactly the UserAgeCategory "Ad" sense above: nothing here
// discovered this person; the user running their own copy is the only user
// there is, and the id and name are a stand-in issued locally, the way a real
// headset issues them from its signed-in account. Non-zero is the whole point
// of the id: 0 is ovrID's own "no such user" and UE4's OnlineSubsystem reads
// it as "nobody is signed in", which is a quit. The name is what
// ovr_User_GetOculusID hands the HUD.
#define KLPLAT_USER_ID   0x4B4C450000000001ull  // "KLE\0..." + 1
// One token and no space, which is what an Oculus ID actually is — the field is
// a handle, not a display name, and every guest that shows it filters it the
// way a handle gets filtered. This was "Klepton User" until Pinball FX VR put
// it in a high-score name box, where the game's own filter dropped the space
// and truncated the rest into an unreadable smear. Nothing here discovered a
// person and nothing here should pretend to have one, so the honest stand-in is
// the generic word for the person holding the controller.
#define KLPLAT_USER_NAME "Player"
// ...and its "access token", which UE4's identity login pairs with the user
// id to build its credentials. This is NOT a credential: it authenticates
// against no service (there is none here), it is never sent anywhere, and its
// value says so. UE4's login flow only checks that one EXISTS before it will
// report the local player as logged in, so the string is a placeholder in the
// same sense the user id is a placeholder — declared, and visibly so.
#define KLPLAT_USER_TOKEN "klepton-local-not-a-real-token"

// A malloc'd list, because two of the message shapes own variable-sized
// payloads (an item array, the cloud directory's string)
// and ovr_FreeMessage is where that ownership ends. The mutex is not defensive
// — the requests are issued from managed code on the main thread and the pump
// runs there too, but nothing in the API promises that.
//
// `str` is the STRING payload, for the messages whose managed wrapper is a
// `Message<string>` — it is read through ovr_Message_GetString rather than
// through the int payload above, and a message carries one or the other.
// `items`/`count` are an array payload (the empty purchase array is the only
// one, and it is empty); both are NULL/0 for every other shape. Everything
// pointed to is malloc'd and freed by ovr_FreeMessage.
typedef struct {
    uint64_t  asset_id;
    char     *filepath;                    // absolute, malloc'd
} klplat_asset;

typedef struct klplat_msg {
    uint32_t            type;
    uint64_t            request_id;
    int32_t             payload;
    size_t              count;
    klplat_asset       *items;
    char               *str;               // ovr_Message_GetString payload, malloc'd
    // A FAILED request's completion. `is_error` is what ovr_Message_IsError
    // answers; `err` is a static string (never freed) that ovr_Error_GetMessage
    // hands back. An error message carries no payload of any other kind, which
    // is the whole of what makes it safe — see klplat_request_error.
    int                 is_error;
    const char         *err;
    struct klplat_msg  *next;
} klplat_msg;
static klplat_msg     *g_msg_head, *g_msg_tail;
static uint64_t        g_next_request = 1;
static pthread_mutex_t g_msg_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        klplat_enqueue(klplat_msg *m);

// Issue a request and queue its completion. Returns the ovrRequest the guest
// will wait on — non-zero, and matched by the message, which together are the
// whole point.
static uint64_t klplat_request_str(const char *what, uint32_t type,
                                   int32_t payload, const char *str) {
    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = type;
    m->payload = payload;
    if (str) m->str = strdup(str);
    uint64_t id = klplat_enqueue(m);
    fprintf(stderr, "  [plat] %s -> request %llu, completion queued "
                    "(type 0x%08x, payload %d%s%s)\n",
            what, (unsigned long long)id, type, payload,
            str ? ", string " : "", str ? str : "");
    return id;
}

// ...and the int-payload spelling, which is all but one caller.
static uint64_t klplat_request(const char *what, uint32_t type, int32_t payload) {
    return klplat_request_str(what, type, payload, NULL);
}

// ---------------------------------------------------------------------------
// A request that FAILED — and the round-16 finding, which is that answering 0
// and queueing nothing is not the cheap end of the same behaviour. It is a
// PERMANENT LEAK in the guest, and on Pinball FX it is a process-lifetime hang.
//
// The old reasoning was "a fabricated request id would have the caller polling
// ovr_PopMessage for a completion that cannot come". The premise is false: a
// completion CAN come — an error one. What actually cannot come is the thing
// the old answer promised, which is nothing at all.
//
// What the guest does with 0, measured on the device (round 16, 2026-08-22):
// UE's FOnlineSubsystemOculus registers a completion delegate under the
// ovrRequest it is handed, WITHOUT checking it against 0, and retires it only
// when a message carrying that id is popped. `ovr_Leaderboard_WriteEntry -> 0,
// no completion queued` therefore leaves a delegate — and the pending-request
// object it keeps alive — resident for the life of the process. Zen Studios'
// Exit-Table teardown ends in an unbounded drain on the game thread:
//
//     while (__atomic_load_8(&outstanding) != 0) FPlatformProcess::Sleep(0.1f);
//
// (libUnreal.so+0x4a67748, reached from the teardown at +0x4d57e2c through the
// Blueprint VM). Nothing of ours is held and nothing of ours is blocked; the
// compositor keeps re-presenting the last guest frame, which is the black
// square. Four stall dumps twenty seconds apart show the identical stack and an
// ovrp call total flat at 3530217. The guest does not die. It never leaves.
//
// So on a run that has DECLARED THE PLATFORM UP, every request must complete.
// That is not a courtesy, it is the contract the declaration created: a real
// initialised platform never answers 0 to a request — it answers a request id
// and delivers a message, which for an unreachable service is an ERROR message.
// Returning 0 after init succeeded is the one shape a real platform cannot
// produce, and it is the shape that strands the caller.
//
// **This grants nothing.** An error message has no payload: `payload` is 0,
// `str` is NULL, `items` is NULL, and every typed accessor a guest might reach
// for over it (ovr_Message_GetLeaderboardEntryArray, ovr_Message_GetProductArray,
// ovr_Message_GetPurchaseArray, ovr_Message_GetAchievementProgressArray, ...)
// is absent from both name tables and still lands on klplat_called, which is
// still fatal by name. The ownership carve-out is untouched: klplat_drm still
// aborts on every entitlement ANSWER, and this changes only whether a REQUEST
// that was already refused is allowed to finish being refused.
//
// Gated on g_platform_up rather than made unconditional, because a run that has
// truthfully reported the platform DOWN owes nobody a completion and its guest
// is on its own documented error path (klplat_init_fails). Only a plat_init
// target reaches here — deadpool and pinballfx today, both UE.
//
// ovr_Message_GetType answers 0 (ovrMessage_Unknown) rather than a fabricated
// per-request type: UE dispatches on the REQUEST ID and removes the delegate
// before it ever looks at the type, so the type is not load-bearing, and
// claiming to be a Leaderboard_WriteEntry completion would be the invention
// this deliberately avoids. Note for a future Unity plat_init target: Unity's
// SDK also dispatches on request id, but it builds a typed Message<T> from the
// type first, so that port should revisit this line before it ships.
static uint64_t klplat_request_error(const char *name, const char *why) {
    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = 0;                     // ovrMessage_Unknown — see above
    m->is_error = 1;
    m->err = "Klepton: there is no Oculus platform service on this host, so the "
             "request could not be made. Nothing was reported.";
    uint64_t id = klplat_enqueue(m);
    if (why)
        fprintf(stderr, "  [plat] %s -> request %llu, ERROR completion queued "
                        "(%s). The request is reported FAILED rather than left "
                        "outstanding — a request the guest never retires is a "
                        "leak it waits on at teardown\n",
                name, (unsigned long long)id, why);
    return id;
}


// The accessors the SDK's Message base class and the two payload wrappers call
// on that handle. Real functions rather than named trampolines, because these
// take the handle in x0 and a trampoline would put the entry point's NAME there.
//
// ovr_Message_GetError is deliberately absent: nothing here is ever an error, so
// the guest never asks, and the fail-closed default would name it if that
// changed. Same for every other Message accessor — a message shape we do not
// produce should stop the run by name rather than answer.
static uint64_t klplat_Message_GetType(const klplat_msg *m) {
    plat_hit("ovr_Message_GetType");
    return m ? m->type : 0;
}
static uint64_t klplat_Message_IsError(const klplat_msg *m) {
    plat_hit("ovr_Message_IsError");
    return m && m->is_error;
}
// The error handle IS the message, for klplat_Message_GetPayload's reason:
// there is one error per message, so a second allocation would only be a second
// thing to keep alive. NULL for a message that is not an error, which is what
// the SDK's own contract says and what a caller that checks IsError first will
// never see.
static void *klplat_Message_GetError(klplat_msg *m) {
    plat_hit("ovr_Message_GetError");
    return (m && m->is_error) ? m : NULL;
}
static const char *klplat_Error_GetMessage(const klplat_msg *m) {
    plat_hit("ovr_Error_GetMessage");
    return m ? m->err : NULL;
}
static const char *klplat_Error_GetDisplayableMessage(const klplat_msg *m) {
    plat_hit("ovr_Error_GetDisplayableMessage");
    return m ? m->err : NULL;
}
// A code and an HTTP code, because a guest that logs the error usually reads
// them and an unimplemented name here would trade the hang for an abort. Both
// report "this did not work"; neither reports anything about the request.
static uint64_t klplat_Error_GetCode(const klplat_msg *m) {
    plat_hit("ovr_Error_GetCode");
    (void)m;
    return 1;                        // ovrError_Unknown
}
static uint64_t klplat_Error_GetHttpCode(const klplat_msg *m) {
    plat_hit("ovr_Error_GetHttpCode");
    (void)m;
    return 0;                        // no transport was involved at all
}
static uint64_t klplat_Message_GetRequestID(const klplat_msg *m) {
    plat_hit("ovr_Message_GetRequestID");
    return m ? m->request_id : 0;
}
// The payload handle IS the message: there is one payload per message here, so
// a separate allocation would only be a second thing to keep alive.
static uint64_t klplat_Message_GetPayload(const klplat_msg *m) {
    plat_hit("ovr_Message_GetPayload");
    return (uint64_t)(uintptr_t)m;
}
static uint64_t klplat_payload_int(const klplat_msg *m) {
    plat_hit("ovr_Payload_GetInt");
    return m ? (uint64_t)(uint32_t)m->payload : 0;
}
// ...and the string one. `Message<string>` reads its data with this rather than
// through the payload handle, so a message whose managed wrapper is a string
// carries `str` and answers here. NULL for every other message, which is what
// the SDK expects for one that has no string.
static const char *klplat_Message_GetString(const klplat_msg *m) {
    plat_hit("ovr_Message_GetString");
    return m ? m->str : NULL;
}

// ---------------------------------------------------------------------------
// What we do implement: the platform reporting its own absence, honestly.
//
// ovr_IsPlatformInitialized answers false, because it is not initialised and
// never will be. That is the same answer a real headset would give before init,
// so the guest's own "platform unavailable" path is a path it already has — its
// metadata carries "Oculus Platform failed to initialize." and
// "Initialize Error: Oculus platform failed to initialize due to exception."
// ...and it answers TRUE once an init request has been completed, because the
// SDK gates every other request on it — `Entitlements.IsUserEntitledToApplication`
// and `Users.GetUserAgeCategory` both begin `if (Core.IsInitialized())`, and
// Core.IsInitialized() IS this call. Answering false there is not a cautious
// answer, it is a guarantee that no request is ever made and so no completion
// is ever awaited-and-delivered; the two halves have to agree.
static uint64_t klplat_IsPlatformInitialized(void) {
    plat_hit("ovr_IsPlatformInitialized");
    return (uint64_t)g_platform_up;
}

// ...and the SYNCHRONOUS Android wrapper, which is the same decision reached
// through a different door — and a different RETURN TYPE, which is what made it
// worth measuring rather than grouping.
//
// `Oculus.Platform.Core.Initialize(appId)` calls this one and THROWS when it is
// false: `UnityException("Oculus Platform failed to initialize.")`. It is not an
// ovrRequest and not a PlatformInitializeResult — the real export builds a byte
// (`and w8, w8, #1; strb w8, [sp, ...]`), i.e. a plain bool, checked in both
// this tree's loaders (superhot +0x51934, beatsaber +0x8aae4). Grouped with the
// request-returning init family it would answer 0, and 0 in a bool is false.
//
// Why it answers TRUE, given the file's standing objection to inventing success:
// that objection is about a request id nothing completes, and it no longer
// applies. This branch has a message queue, the async init already answers
// success and sets g_platform_up, and the app's own entitlement completes
// through it. Answering false HERE while answering success THERE is the two
// halves disagreeing — and the cost of the disagreement is not an error path,
// it is the whole title: a guest running the stock
// `Oculus.Platform.Samples.EntitlementCheck` behaviour catches the throw, runs
// its failure handler, and calls `UnityEngine.Application.Quit()`. Unity's quit
// gate then makes `nativeRender` a no-op for the rest of the process's life, so
// what a person sees is not an error message but a frozen frame, reprojected
// forever, with every counter healthy. SUPERHOT VR is such a guest.
//
// **The DRM line does not move with this.** Asking to connect to the platform is
// not an ownership question (that is why this family was never among the
// refusals); IAP, purchase records, asset details and every delivery call still
// abort unconditionally and check_drm_guard still asserts both directions.
// What init being answered buys is that the app's own licence check — the one
// carve-out this project deliberately made — becomes REACHABLE. A guest that
// quits before it can ask is not a guest being refused; it is one that never got
// to the question.
//
// Deliberately narrow: only this name. The Asynchronous / Standalone spellings
// have their own answers above, and the `ovr_PlatformInitialize*` family returns
// an `ovrPlatformInitializeResult` (a THIRD convention, whose Success is 0).
static uint64_t klplat_UnityInitWrapper(const char *app_id) {
    plat_hit("ovr_UnityInitWrapper");
    g_platform_up = 1;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_UnityInitWrapper(\"%s\") -> true (the "
                        "synthetic platform reports it came up, so the app's own "
                        "entitlement check can be asked and answered; every "
                        "ownership query still refuses)\n", app_id ? app_id : "");
    }
    return 1;
}

// The app's own licence check — see the carve-out comment above for why this
// one is answered and the rest of the family is not: the user unpacked their
// own APK, so a licence to the application itself is something we can assert,
// while DLC needs out-of-band asset data and a licence we cannot verify.
// Non-zero is load-bearing rather than incidental — 0 here
// is the guest's licence-FAILURE path, not a neutral answer — so m_boot's
// check_drm_guard asserts the value, in both directions along with the
// refusals it sits beside.
// It is a REQUEST, not a predicate — `Entitlements.IsUserEntitledToApplication()`
// wraps its return in a `Request` and awaits the completion, so the old bare 1
// was request id 1 with nothing ever arriving for it. The answer is unchanged
// and so is the reasoning above; it is now delivered where the guest reads it,
// as a non-error completion, which is how this API spells "entitled".
// The platform's asset-file list. On a real Quest this is answered LOCALLY,
// offline included: the platform reports which asset files are installed, and
// for this title - whose whole 16 GB ships in the obb - the true answer is an
// EMPTY list. Klepton used to refuse the request outright (request id 0), the
// SDK surfaced "Request failed", and the game's loading orchestrator waited on
// a content check that could never complete. An empty list delivers no
// content, so the DRM line does not move: delivery, purchase and IAP queries
// keep refusing.
static uint64_t klplat_AssetFile_GetList(void) {
    plat_hit("ovr_AssetFile_GetList");
    return klplat_request("ovr_AssetFile_GetList", KLPLAT_MSG_ASSET_LIST, 0);
}

// The Oculus user *access token* — an opaque credential the app hands to its own
// backend (Ubisoft Connect) to open a session. It is NOT an ownership answer: the
// entitlement check is a separate request (klplat_Entitlement_GetIsViewerEntitled
// above) and this token grants nothing on its own. Returning 0 — "the request
// could not be made" — is what we did before, and AC Nexus does not treat it as a
// soft failure: its platform-init chain logs "Request failed" and the loading->menu
// transition aborts back to the loading loop. So we COMPLETE the request instead,
// with an opaque offline token. With KL_NET_OFFLINE the token is never exchanged
// against a server; the only point is that the request the game gates its own menu
// on RESOLVES. Read back through ovr_Message_GetString, like CloudStorage2's path.
static uint64_t klplat_User_GetAccessToken(void) {
    plat_hit("ovr_User_GetAccessToken");
    static const char tok[] = "OCACkleptonOfflineAccessToken000000000000";
    return klplat_request_str("ovr_User_GetAccessToken",
                              KLPLAT_MSG_ACCESS_TOKEN, 0, tok);
}

// Achievement *definitions* — the catalogue the game's achievement manager reads
// at startup. No Oculus service here defines any, so the honest answer is an EMPTY
// set, the same shape ovr_AssetFile_GetList's empty list takes. Delivered as a
// completed request (IsError false, an AchievementDefinitionArray of size 0) rather
// than as 0/"request could not be made", because AC Nexus gates its menu on the
// request resolving and a bare 0 lands it on the same "Request failed" path that
// aborts the transition. Achievements record what a player has DONE, not what they
// own, so an empty definition set invents no entitlement. The size-0 array is read
// through GetAchievementDefinitionArray (-> the message payload) and the existing
// Array_GetSize answer (plat_is_empty_array -> 0).
static uint64_t klplat_Achievements_GetAllDefinitions(void) {
    plat_hit("ovr_Achievements_GetAllDefinitions");
    return klplat_request("ovr_Achievements_GetAllDefinitions",
                          KLPLAT_MSG_ACHIEVEMENT_DEFS, 0);
}

// Who is signed in — the ASYNC request form (the synchronous ovr_GetLoggedInUserID
// is separate, below). Its completion is Message<User>. By DEFAULT we still answer
// 0, "the request could not be made", which is the offline path Beat Saber wants
// (see g_plat_request). But a title can run its online-init as a chain that ABORTS
// on a failed request: AC Nexus's log shows the Oculus entitlement check PASS and
// then this call fail with "Request failed", stranding it on the loading screen.
// KL_PLAT_USER opts such a title into a synthetic OFFLINE user so the chain
// completes. Not an ownership answer — entitlement is a separate request
// (klplat_Entitlement_GetIsViewerEntitled) that already passed — only "who is
// playing", which a real headset answers even with no network.
// Default ON for titles that HARD-QUIT on a failed login rather than falling
// back to offline. ZIX prints "Oculus Auth Failed! Quitting" and exits when
// GetLoggedInUser returns 0, so for it the synthetic offline user is not
// optional; the knob still overrides in either direction. Beat Saber and the
// rest keep the default-0 offline path (they tolerate no user).
static int klplat_user_default(void) {
    // Titles that ABORT when ovr_GetLoggedInUserID / GetLoggedInUser returns 0 —
    // they put up "Unable to get a valid UserID ... check you have an Entitlement"
    // and then FMessageDialog->abort on the game thread. A synthetic offline user
    // is strictly better for these: there is no Oculus service to sign in to, and
    // the alternative is a hard exit before the menu. Unity titles that read 0 as
    // "offline, carry on" are NOT in this list and keep the 0.
    static const char *on[] = { "zix", "intotheradius", "redmatter2", "vampire", "twd2" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    return 0;
}
static uint64_t klplat_User_GetLoggedInUser(void) {
    plat_hit("ovr_User_GetLoggedInUser");
    if (!kl_env_on("KL_PLAT_USER", klplat_user_default())) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [plat] ovr_User_GetLoggedInUser -> 0 (no platform "
                    "user; set KL_PLAT_USER=1 for a synthetic offline user)\n"); }
        return 0;
    }
    return klplat_request("ovr_User_GetLoggedInUser", KLPLAT_MSG_LOGGED_IN_USER, 0);
}

// The User that completion carries. The message handle IS the user handle (one
// payload per message), and these read its fields. An offline user: a fixed
// non-zero ovrID — 0 would read as "no user" and defeat the point — and a plain
// name. Every other User field the SDK's model reads is optional, and NULL/0 is
// how it spells "absent" (see the launch-details note), so only these three are
// set; the rest answer 0 permissively and are never even reached unless
// KL_PLAT_USER handed out the user above.
static uint64_t klplat_User_GetID(const void *h) {
    plat_hit("ovr_User_GetID"); (void)h;
    return 1000000000000042ull;          // fixed, non-zero synthetic ovrID
}
static const char *klplat_User_GetOculusID(const void *h) {
    plat_hit("ovr_User_GetOculusID"); (void)h;
    return "kleptonplayer";
}
static const char *klplat_User_GetDisplayName(const void *h) {
    plat_hit("ovr_User_GetDisplayName"); (void)h;
    return "Klepton Player";
}
// The list payload's one accessor pair: the handle is the message (the same
// convention as every other payload here), and its size is zero.
static uint64_t klplat_AssetFileDeliveryList_GetSize(const void *h) {
    plat_hit("ovr_AssetDetailsArray_GetSize");
    (void)h;
    return 0;
}

static uint64_t klplat_Entitlement_GetIsViewerEntitled(void) {
    plat_hit("ovr_Entitlement_GetIsViewerEntitled");
    return klplat_request("ovr_Entitlement_GetIsViewerEntitled",
                          KLPLAT_MSG_ENTITLEMENT, 0);
}

// Where the user's saves live — and the one member of the CloudStorage family
// that is answered rather than refused.
//
// The rest of that family stays refused, and the split is the same one the DRM
// line is drawn along: `ovr_CloudStorage_Load` / `_Save` move DATA to and from a
// service that is not here, so 0 ("the request could not be made") is the truth
// about them. This one moves no data at all. It asks where on the local
// filesystem the save directory IS — the directory the platform would later
// synchronise — and that is a question this host can answer completely.
//
// Refusing it is what a black launch screen looks like. SUPERHOT's
// `VRSaveManager::LoadAsync` chains its whole load off this request, so a 0
// leaves the SDK waiting on a completion it will never post: no save data, no
// scene, and the launch precache's own `ClearRenderTarget` command buffer parked
// on the camera in the meantime — which renders as black, forever, with every
// frame counter healthy. "A game that cannot reach the cloud writes locally" is
// only true if something tells it where local is.
//
// It is a REQUEST returning `Request<string>` (the SDK's `MessageWithString`),
// so the path arrives as a message payload read through ovr_Message_GetString —
// not as a return value. The type number is the guest's own, read out of the
// running IL2CPP runtime with KL_PROBE_ENUM like the three above it.
//
// The directory is created, and that is not a convenience: the guest calls its
// own IsDirectoryWritable on the answer before it uses it, and a path that does
// not exist fails that test — which is the same stall wearing a different hat.
// It sits beside the guest's files directory, so it is per-target and follows
// KL_FILES_DIR like everything else the guest writes.

// The age category, and the ONE answer in this file that is a declaration
// rather than a measurement. See the completion-queue comment above: the user
// declared themselves over 18 and this reports it; nothing here discovered it.
// `Ad` is Oculus.Platform.AccountAgeCategory's own spelling, read out of the
// guest, and Beat Saber maps it to its `UserAgeCategory.Adult`.
static uint64_t klplat_UserAgeCategory_Get(void) {
    plat_hit("ovr_UserAgeCategory_Get");
    return klplat_request("ovr_UserAgeCategory_Get",
                          KLPLAT_MSG_AGE_CATEGORY, KLPLAT_AGE_ADULT);
}

// The application's own version, which is the one question in this family we can
// answer from something we actually have: the unpacked tree's apktool.yml, i.e.
// exactly what PackageManager already tells the guest. `OculusInit`'s
// GetAppVersionQuestAsync asks it during startup, behind the init completion.
//
// LATEST is answered as CURRENT, and that is a statement rather than a dodge:
// there is no store here to ask what the newest build is, and "the installed
// one is the newest one" is the only self-consistent thing to say — it means
// "no update available", which is true of this host in the only sense available
// to it. Reporting a HIGHER latest would advertise an update that cannot be
// fetched, which is what drives Application_StartAppDownload.
static uint64_t klplat_Application_GetVersion(void) {
    plat_hit("ovr_Application_GetVersion");
    return klplat_request("ovr_Application_GetVersion", KLPLAT_MSG_APP_VERSION, 0);
}

static uint64_t klplat_AppVersion_code(const void *h) {
    plat_hit("ovr_ApplicationVersion_GetCode");
    (void)h;
    long code = 0; kl_jni_guest_version(&code, NULL);
    return (uint64_t)(int64_t)code;
}
// The two fields of the same model we cannot answer from anything here. A
// release date and a download size are properties of a STORE listing, and there
// is no store; 0 is the SDK's own "not published" rather than a stand-in, and
// the model reads every field at construction whether or not it uses them.
static uint64_t klplat_AppVersion_unknown(const void *h) {
    plat_hit("ovr_ApplicationVersion_unknown_field");
    (void)h;
    return 0;
}

static const char *klplat_AppVersion_name(const void *h) {
    plat_hit("ovr_ApplicationVersion_GetName");
    (void)h;
    const char *name = NULL; kl_jni_guest_version(NULL, &name);
    return name ? name : "";
}

// The ASYNCHRONOUS init. Its completion is what everything else waits behind,
// so this is also where the platform becomes "up" for ovr_IsPlatformInitialized.
// Only the async spellings come here: the synchronous ones return a
// PlatformInitializeResult directly and keep their old answer.
static uint64_t klplat_init_async(const char *name) {
    plat_hit(name);
    g_platform_up = 1;
    return klplat_request(name, KLPLAT_MSG_PLATFORM_INIT, KLPLAT_INIT_SUCCESS);
}

static const char *const g_plat_init_async[] = {
    "ovr_UnityInitWrapperAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronousWithOptions",
    "ovr_PlatformInitializeWindowsAsynchronous",
};

static int plat_is_init_async(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_init_async / sizeof g_plat_init_async[0]; i++)
        if (strcmp(g_plat_init_async[i], name) == 0) return 1;
    return 0;
}


// Unity's native plugin interface. Every entry point in it returns void, and the
// real platform loader uses them only to keep hold of the IUnityInterfaces
// registry and the render event queue — there is nothing here to bind either to.
// Taken as a group because it is one fixed interface Unity calls on every plugin,
// and the same group is already answered this way in kl_ovrp.c.
static uint64_t klplat_void(const char *name) {
    plat_hit(name);
    return 0;
}

static const char *const g_plat_unity[] = {
    "UnityPluginLoad", "UnityPluginUnload", "UnitySetEventQueue",
    "UnitySetGraphicsDevice", "UnityShaderCompilerExtEvent", "UnityRenderEvent",
    // The rendering-extension pair. UnityRenderingExtEvent is void; the Query
    // returns int, and 0 is the truthful answer — this plugin wants no
    // render-thread extension events, so Unity will never issue them.
    "UnityRenderingExtEvent", "UnityRenderingExtQuery",
    // Audio-plugin enumeration, dlsym'd speculatively by libunity's
    // AudioPluginManager on every plugin handle. A COUNT of effect definitions,
    // so 0 is "none here" — which is what the real loader says by not exporting
    // it. Same answer, same reasoning, as kl_ovrp.c's copy.
    "UnityGetAudioEffectDefinitions",
    // ovr_FreeMessage is NOT here: it has real work to do (the enumeration
    // messages are heap-allocated), so it routes to a real free above.
};

static int plat_is_unity_hook(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_unity / sizeof g_plat_unity[0]; i++)
        if (strcmp(g_plat_unity[i], name) == 0) return 1;
    return 0;
}

// Platform initialisation. Note this is *not* one of the refused calls: asking to
// connect to the platform is not an ownership question, and it has a truthful
// answer here — it fails, because there is no com.oculus.horizon to connect to.
//
// An ovrRequest of 0 is the SDK's "the request could not be made". That is the
// honest report, and it lands the guest on an error path it already has: its own
// metadata carries "Oculus Platform failed to initialize." and "Initialize Error:
// Oculus platform failed to initialize due to exception."
//
// Returning a plausible request id instead would be a lie with a tail: the caller
// would then poll ovr_PopMessage forever for a completion that cannot come.
//
// EXCEPT on a target that bakes KL_PLAT_INIT=1 (deadpool — see targets.py).
// There the truthful failure is the title's whole boot: PILO calls the
// SYNCHRONOUS ovr_PlatformInitializeAndroid, reads the user id, and
// ForceQuits when OnlineSubsystemOculus reports the platform unusable — a
// guest that quits before it can ask its own entitlement question is not a
// guest being refused; it is one that never got to the question. So for such a
// target this reports the platform UP: g_platform_up is set so
// ovr_IsPlatformInitialized agrees, and the RETURN VALUE — 0 — is already
// Success, because the ovr_PlatformInitialize* family returns an
// ovrPlatformInitializeResult whose Success IS 0. No completion is owed: the
// synchronous spelling's contract is the return value, not a message. Content
// delivery still refuses, and check_drm_guard still asserts both directions.
//
// (Was a `plat_init` column on the deadpool branch; driver's table carries
// per-target policy as the row's `env` string, so it crossed as a knob — the
// name PORTING.md already gave it. Read once: resolution happens after
// kl_target_use / the env file have applied the row.)
static int klplat_init_target(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_PLAT_INIT", 0) ? 1 : 0;
    return v;
}
static uint64_t klplat_init_fails(const char *name) {
    plat_hit(name);
    if (klplat_init_target()) {
        g_platform_up = 1;
        static int said_up;
        if (!said_up) {
            said_up = 1;
            fprintf(stderr, "  [plat] %s -> Success (KL_PLAT_INIT: the "
                            "synthetic platform reports it came up, so the app's "
                            "own entitlement check can be asked and answered; "
                            "every ownership query still refuses)\n", name);
        }
        return KLPLAT_INIT_SUCCESS;
    }
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1)
        fprintf(stderr, "  [plat] %s -> 0 (no Oculus platform service on this host; "
                        "reporting failure rather than inventing success)\n", name);
    return 0;
}

// ...and the ordinary request calls, which share the return value (0) but NOT
// the claim. klplat_init_fails answers for the ovr_PlatformInitialize* family,
// where 0 is Success and a plat_init target has to report the platform up. A
// leaderboard fetch or a rich-presence set is not an init call: its 0 means
// "the request could not be made", it owes no completion, and it must never be
// the thing that declares the platform up — which it silently was, for any
// request that happened to be asked before the init call on a plat_init target.
//
// Same distinction klplat_drm_request draws for the ownership families' request
// half, for the same reason: the platform's state is not what is in question
// here, the request's is. One line per NAME rather than one for the whole set,
// because the next round reads this log to find out which call came last.
//
// Round 16: on a run that has declared the platform up, this now reports the
// request FAILED rather than leaving it outstanding — see klplat_request_error
// for the device evidence and for why 0-with-no-message is the one answer a
// real initialised platform never gives. Nothing is reported either way; the
// accessors over it stay unreachable either way. The only difference is that
// the guest gets to retire its own pending request.
static uint64_t klplat_request_fails(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) g_plat[s].calls++;
    int first = (s < 0 || g_plat[s].calls == 1);
    // Gated on KL_PLAT_INIT (the Unreal plat_init targets), NOT on
    // g_platform_up alone: a Unity guest also brings the platform up
    // (ovr_UnityInitWrapper / the async init), and for Unity a type-0 queued
    // error stalls the SDK's message pump — see "Why this branch does NOT
    // queue an ERROR message" below. Unity keeps driver's bare 0.
    if (g_platform_up && klplat_init_target())
        return klplat_request_error(name, first
            ? "no Oculus service on this host. Nothing is reported and the "
              "accessors over it are still unreachable"
            : NULL);
    if (first)
        fprintf(stderr, "  [plat] %s -> 0 (request NOT made, and the platform has "
                        "not been declared up: no Oculus service on this host. "
                        "Nothing is reported and no completion is queued, so the "
                        "accessors over it are unreachable)\n", name);
    return 0;
}

// ovr_UnityInitWrapper is NOT here — it is a bool, not an ovrRequest, and it has
// its own implementation above. It is the only one of these whose contract can
// report success without owing anybody a completion message.
static const char *const g_plat_init[] = {
    "ovr_UnityInitWrapperAsynchronous",
    "ovr_UnityInitWrapperStandalone", "ovr_UnityInitGlobals",
    "ovr_PlatformInitializeAndroid", "ovr_PlatformInitializeAndroidAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronousWithOptions",
    "ovr_PlatformInitializeWithAccessToken", "ovr_PlatformInitializeStandaloneOculus",
};

static int plat_is_init(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_init / sizeof g_plat_init[0]; i++)
        if (strcmp(g_plat_init[i], name) == 0) return 1;
    return 0;
}

// Request-returning calls with the same truthful answer as init: 0, "the
// request could not be made", because there is no platform service to make it
// of. Answering with a fabricated request id would have the caller polling
// ovr_PopMessage for a completion that cannot come.
static const char *const g_plat_request[] = {
    // The IAP catalog (see plat_is_iap_catalog): a store query with no store to
    // ask, so 0 = "request could not be made", the honest failure. Ownership IAP
    // calls never reach here — plat_is_drm refuses them first.
    "ovr_IAP_GetProductsBySKU", "ovr_IAP_GetNextProductArrayPage",
    // Owned-inventory query: honest failure, grants nothing (see plat_is_iap_owned_query).
    "ovr_IAP_GetViewerPurchases", "ovr_IAP_GetViewerPurchasesDeltaAsync",
    "ovr_RichPresence_Clear", "ovr_RichPresence_Set",
    "ovr_RichPresence_SetDestination", "ovr_RichPresence_SetIsJoinable",
    // Asking who is logged in is not an ownership question; with no platform
    // service the request cannot be made, which is what 0 says. Beat Saber
    // 1.28 takes its offline path from there (~swap 34k of the boot).
    "ovr_User_GetLoggedInUser",

    // ---- the rest of the ovr_User_* REQUEST half (batman, boot #3) ----
    //
    // Arkham Shadow's OculusCorePlatformProvider.InitPlatform() walks this
    // family during startup and boot #3 died on the first member of it:
    //
    //   [plat] ovr_Achievements_GetAllDefinitions -> 0 (...)
    //   E/Unity: An unknown error occurred. Request failed.       <- handled
    //   ...
    //   [klepton] fatal: ... unimplemented ... 'ovr_User_GetAccessToken'
    //
    // Those two lines are the whole argument. `ovr_Achievements_*` already
    // answers 0 here, and the guest's own SDK turned that into a completed,
    // FAILED request client-side and carried on for another dozen log lines —
    // which is exactly the outcome an "error completion" is supposed to buy.
    // The Unity SDK special-cases an ovrRequest of 0 in Callback.OnComplete and
    // fires the callback immediately with a synthesized error ("An unknown
    // error occurred. Request failed." is its own string, and it is in this
    // guest's global-metadata.dat); nothing is left outstanding and nothing
    // waits on the pump. See the note below g_plat_request_prefix for why the
    // Unreal branches (pinballfx, deadpool) need a QUEUED error message instead
    // and why queueing one here would be a regression.
    //
    // NOT a blanket ovr_User_ prefix: this family is the mixed one the comment
    // below calls out — ovr_User_GetID / _GetOculusID / _GetDisplayName and the
    // rest of the accessors read fields off a User handle and a blanket 0 would
    // be a wrong ANSWER rather than a refused request. Every name here was
    // cross-checked against the guest's OWN Message.MessageType enum in
    // global-metadata.dat (the same table that yielded the type constants
    // above): a name is a request exactly when the enum has a member for it,
    // and all ten below do. Refusing them reports what a headset with no
    // network and no signed-in session reports; none of them is an ownership
    // question (IAP / Purchase / Entitlement / AssetFile still refuse through
    // plat_is_drm, which runs first), and an access token we do not have is
    // reported as absent rather than invented.
    "ovr_User_Get",
    "ovr_User_GetAccessToken",
    "ovr_User_GetBlockedUsers",
    "ovr_User_GetLinkedAccounts",
    "ovr_User_GetLoggedInUserFriends",
    "ovr_User_GetOrgScopedID",
    "ovr_User_GetSdkAccounts",
    "ovr_User_GetUserCapabilities",
    "ovr_User_GetUserProof",

    // ---- the HTTP fetch half ----
    //
    // The SDK's own transport, not a game feature: `ovr_HTTP_GetWithMessageType`
    // is how every DeserializableList<T> fetches its next page, and HTTP_Get /
    // GetToFile / Post / MultiPartPost are the plain fetches behind avatar
    // images and the like. All five have a MessageType member in this guest's
    // enum, so all five are requests. There is no service and no session to
    // fetch against, so 0 — "the request could not be made" — is the literal
    // truth, and it lands on the same handled-failure path as the achievements
    // line above.
    //
    // The STREAMING trio (ovr_HTTP_StartTransfer / _Write / _WriteEOM) is
    // deliberately absent: it is a different shape (a transfer id plus chunked
    // writes, delivered through ovr_HttpTransferUpdate_*), it has no MessageType
    // member, and nothing has asked for it. It keeps the fail-closed default.
    "ovr_HTTP_Get",
    "ovr_HTTP_GetToFile",
    "ovr_HTTP_GetWithMessageType",
    "ovr_HTTP_MultiPartPost",
    "ovr_HTTP_Post",

    // ---- the rest of the surface, derived rather than discovered (pttr) ----
    //
    // Paint the Town Red VR died on ovr_Leaderboard_GetEntries after a boot that
    // had already composited a frame, and it was the fourth launch in a row lost
    // to "the next name nobody implemented". The surface does not run dry that
    // way: this guest's libil2cpp.so carries the SDK's ENTIRE C binding — 700
    // distinct ovr_* entry points in its rodata — because Unity compiles CApi.cs
    // whether the title calls it or not.
    //
    // So the split was derived in one pass instead of one launch at a time, by
    // the test this file already established and trusts: **a name is a request
    // exactly when the guest's own ovrMessageType enum has a member for it.**
    // The enum members live in global-metadata.dat's identifier heap, which
    // work/pttr-jni-census.md already parses; intersecting the 700 rodata names
    // with the 153,246 identifiers yields 167 requests, and the residue is
    // accessors, options builders and commands. It reproduces the nine
    // ovr_Leaderboard_* the pinballfx branch worked out by hand from the
    // loader's thunks (d5d0172) name for name, which is the check that the test
    // is measuring the right thing.
    //
    // Listed BY NAME, never by prefix, for every family below — the mixed ones
    // are exactly the families the prefix table's comment warned about, and
    // ovr_Challenge_GetID or ovr_Party_GetID answering "0" would be a wrong
    // ANSWER rather than a refused request. Names already covered elsewhere are
    // absent by construction: the DRM families (plat_is_drm runs first), the six
    // request PREFIXES below, the ten ovr_User_* above, the five ovr_HTTP_*, and
    // everything in g_plat_impl / g_plat_init.
    //
    // None of these is an ownership question. Leaderboards and challenges record
    // what a player SCORED, parties and NetSync are who they are playing with,
    // Livestreaming and Media are what they broadcast, and the report/block
    // flows are moderation. The entitlement line does not move with any of it.
    "ovr_Leaderboard_Get", "ovr_Leaderboard_GetEntries",
    "ovr_Leaderboard_GetEntriesAfterRank", "ovr_Leaderboard_GetEntriesByIds",
    "ovr_Leaderboard_GetNextEntries", "ovr_Leaderboard_GetPreviousEntries",
    "ovr_Leaderboard_WriteEntry",
    "ovr_Leaderboard_WriteEntryWithSupplementaryMetric",
    "ovr_Challenges_Create", "ovr_Challenges_DeclineInvite",
    "ovr_Challenges_Delete", "ovr_Challenges_Get", "ovr_Challenges_GetEntries",
    "ovr_Challenges_GetEntriesAfterRank", "ovr_Challenges_GetEntriesByIds",
    "ovr_Challenges_GetList", "ovr_Challenges_GetNextChallenges",
    "ovr_Challenges_GetNextEntries", "ovr_Challenges_GetPreviousChallenges",
    "ovr_Challenges_GetPreviousEntries", "ovr_Challenges_Join",
    "ovr_Challenges_Leave", "ovr_Challenges_UpdateInfo",
    "ovr_Party_Create", "ovr_Party_GatherInApplication", "ovr_Party_Get",
    "ovr_Party_GetCurrent", "ovr_Party_GetCurrentForUser", "ovr_Party_Invite",
    "ovr_Party_Join", "ovr_Party_Leave",
    "ovr_NetSync_Connect", "ovr_NetSync_Disconnect", "ovr_NetSync_GetSessions",
    "ovr_NetSync_GetVoipAttenuation", "ovr_NetSync_GetVoipAttenuationDefault",
    "ovr_NetSync_SetVoipAttenuation", "ovr_NetSync_SetVoipAttenuationModel",
    "ovr_NetSync_SetVoipChannelCfg", "ovr_NetSync_SetVoipGroup",
    "ovr_NetSync_SetVoipListentoChannels", "ovr_NetSync_SetVoipMicSource",
    "ovr_NetSync_SetVoipSessionMuted", "ovr_NetSync_SetVoipSpeaktoChannels",
    "ovr_NetSync_SetVoipStreamMode",
    "ovr_Voip_GetMicrophoneAvailability", "ovr_Voip_ReportAppVoipSessions",
    "ovr_Voip_SetSystemVoipSuppressed",
    "ovr_RichPresence_GetDestinations", "ovr_RichPresence_SetLobbySession",
    "ovr_RichPresence_SetMatchSession",
    "ovr_Livestreaming_IsAllowedForApplication",
    "ovr_Livestreaming_StartPartyStream", "ovr_Livestreaming_StartStream",
    "ovr_Livestreaming_StopPartyStream", "ovr_Livestreaming_StopStream",
    "ovr_Livestreaming_UpdateMicStatus",
    "ovr_Application_CancelAppDownload",
    "ovr_Application_CheckAppDownloadProgress",
    "ovr_Application_GetInstalledApplications",
    "ovr_Application_InstallAppUpdateAndRelaunch",
    "ovr_Application_LaunchOtherApp", "ovr_Application_StartAppDownload",
    "ovr_ApplicationLifecycle_GetRegisteredPIDs",
    "ovr_ApplicationLifecycle_GetSessionKey",
    "ovr_ApplicationLifecycle_RegisterSessionKey",
    "ovr_User_CancelRecordingForReportFlow", "ovr_User_LaunchBlockFlow",
    "ovr_User_LaunchFriendRequestFlow", "ovr_User_LaunchReportFlow",
    "ovr_User_LaunchReportFlow2", "ovr_User_LaunchUnblockFlow",
    "ovr_User_NewEntitledTestUser", "ovr_User_NewTestUser",
    "ovr_User_NewTestUserFriends", "ovr_User_StartRecordingForReportFlow",
    "ovr_User_StopRecordingAndLaunchReportFlow",
    "ovr_User_StopRecordingAndLaunchReportFlow2",
    "ovr_User_TestUserCreateDeviceManifest",
    "ovr_UserAgeCategory_Report",
    "ovr_Colocation_GetCurrentMapUuid", "ovr_Colocation_RequestMap",
    "ovr_Colocation_ShareMap",
    "ovr_Avatar_LaunchAvatarEditor", "ovr_Avatar_UpdateMetaData",
    "ovr_AbuseReport_LaunchAdvancedReportFlow",
    "ovr_AbuseReport_ReportRequestHandled",
    "ovr_Notification_MarkAsRead",
    "ovr_Media_ShareToFacebook",
    "ovr_GraphAPI_Get", "ovr_GraphAPI_Post",
    "ovr_DeviceApplicationIntegrity_GetAttestationToken",
    "ovr_DeviceApplicationIntegrity_GetIntegrityToken",

    // The signed-in user's SOCIAL graph — friends, and the room/party a friend
    // is in. Pure social decoration, no ownership: with no platform service the
    // request cannot be made (0). Asgard's Wrath 2 walks this whole family right
    // after KL_PLAT_USER hands it a user; each is honestly "could not be made".
    "ovr_User_GetLoggedInUserFriendsAndRooms",
    "ovr_User_GetLoggedInUserFriendsV2",
    "ovr_User_GetNextUserArrayPage",
    "ovr_User_GetLoggedInUserManagedInfo",
};

// Whole families where EVERY entry point is request-returning, so the same
// answer is right for all of them and an exact list would only stop the run
// once per name for nothing.
//
//   ovr_Achievements_* — progress and definitions for achievements. All nine
//     calls (AddCount, AddFields, Unlock, GetAllDefinitions, GetAllProgress,
//     GetDefinitionsByName, GetProgressByName and the two ArrayPage walkers)
//     return an ovrRequest. This is not an ownership question: achievements
//     record what the player has done, not what they have bought, and 0 says
//     the request could not be made rather than reporting any progress. The
//     entitlement families are unaffected — plat_is_drm runs first.
//   ovr_GroupPresence_* — "who are you playing with", the multiplayer presence
//     the invite/roster panels read. Ownership does not enter into it, and the
//     trailing underscore keeps this clear of ovr_GroupPresenceOptions_* below,
//     which is a different shape entirely.
//   ovr_CloudStorage_* / ovr_CloudStorage2_* — cloud saves. The user's own save
//     data, not a thing they bought. 0 is right for the transfer calls: with no
//     cloud there is nothing to load from or save to. It was NOT right for
//     ovr_CloudStorage2_GetUserDirectoryPath, which is carved out above and
//     answered — see the comment there for the launch it deadlocked. "A game
//     that cannot reach the cloud writes locally" is only true if something
//     tells it where local IS.
//   ovr_LanguagePack_* — which language pack is current. (The _GetCurrent /
//     _SetCurrent pair only; ovr_LanguagePackInfo_* is an accessor family and is
//     answered separately below.)
//   ovr_UserDataStore_* — the per-user key/value store a title keeps progress
//     in. Again the user's own data. Note the trailing underscore keeps this
//     clear of ovr_UserDataStoreUpdateResponse_*, which is an accessor.
//
// Membership here is not a judgement call: for every name under these prefixes
// the game's own metadata carries a matching ovrMessageType, which is what
// "returns an ovrRequest and completes through a message" means. Cross-checked
// name by name against the MessageType enum in global-metadata.dat (the same
// table that yielded ovrMessageType_Entitlement_GetIsViewerEntitled) —
// Achievements 7/7, CloudStorage 9/9, CloudStorage2 1/1, LanguagePack 2/2,
// UserDataStore 8/8. The mixed families (User_, Room_, Leaderboard_, Voip_,
// Party_, Livestreaming_, Application_) are deliberately absent: they mix
// requests with plain accessors, and a blanket 0 would be a wrong ANSWER rather
// than a refused request.
//
// Leaderboard_ was the first of those to be asked for on device (round 6), and
// the prediction held: nine of its twelve exports are requests and three are
// handle accessors. Its request half is now listed BY NAME in g_plat_request
// above — which is what "mixed" was always going to mean in practice, and why
// the family is still not a prefix here.
static const char *const g_plat_request_prefix[] = {
    "ovr_Achievements_", "ovr_GroupPresence_",
    "ovr_CloudStorage_", "ovr_CloudStorage2_",
    "ovr_LanguagePack_", "ovr_UserDataStore_",
};

// ---------------------------------------------------------------------------
// Why this branch does NOT queue an ERROR message for a refused request
//
// The pinballfx branch answers a refused request with a real ovrRequest id and
// a QUEUED error completion (klplat_request_error, c38bfe8/e2351d0), because
// Unreal's FOnlineSubsystemOculus registers a completion delegate under the id
// without checking it against 0 and retires it only when a message carrying
// that id is popped — so a 0 there is a permanent leak, and Zen Studios' Exit
// Table teardown spins forever on the outstanding count. That is a real bug and
// that is a real fix, for a GUEST THAT BEHAVES THAT WAY.
//
// This guest does not. Unity's Oculus SDK special-cases request id 0 in
// Callback.OnComplete and completes the request immediately, client-side, with
// a synthesized error — measured in boot #3 above, where the achievements
// request answered 0 and the very next line was the SDK's own "An unknown error
// occurred. Request failed." with the game carrying on. Nothing is left
// outstanding, so there is nothing for a queued message to retire.
//
// And queueing one would be actively worse here, which is the half worth
// writing down. pinballfx's error message carries type 0 (ovrMessage_Unknown),
// deliberately, because UE dispatches on the request id and never looks at the
// type — and that file's own comment flags the Unity case as needing a revisit
// before it ships. This is that revisit. Unity's Messages.ParseMessageHandle
// switches on ovr_Message_GetType FIRST and builds a typed Message<T> from it;
// its default arm logs "Unrecognized message type" (that string is in this
// guest's global-metadata.dat) and returns null, and Request.RunCallbacks stops
// pumping the moment PopMessage hands it a null. So a type-0 error message
// would not complete the request — it would stall the whole message pump for
// that frame and strand every LATER completion behind it, including the init
// and entitlement ones this file works hard to deliver.
//
// Queueing a CORRECTLY TYPED error would work, but it needs the guest's own
// ovrMessageType constant for each refused call, read out with KL_PROBE_ENUM
// like the five above — a per-name cost with no observed symptom to pay it
// for. If a Unity guest is ever seen waiting on a request that answered 0, that
// is the moment to add klplat_request_error here with a real type, and not
// before.

// ---------------------------------------------------------------------------
// Options objects: ovr_<Thing>Options_Create / _Set<Field> / _Destroy.
//
// Not requests and not queries — a local builder the caller fills in and then
// hands to a request call. Create returns an opaque handle, so 0 is the one
// answer that is NOT safe here: the guest would carry a NULL through to the
// setters, and on a real platform that is a null deref inside the library.
//
// We hand back a non-NULL cookie instead. Nothing reads it: the setters are
// no-ops because the request the options would feed cannot be made anyway
// (ovr_GroupPresence_Set and friends answer 0 above), so there is no state
// worth keeping and one shared cookie serves every live options object. That
// is invented behaviour in the narrow sense, but it invents no ANSWER — the
// guest learns nothing from it that it did not itself put in.
//
// Entitlement spellings never reach here: plat_is_drm runs first, so
// ovr_IAPOptions_* and the rest still refuse.
// Outbound-only calls: the guest hands us something and asks nothing back.
// Accepted and dropped, which is not a stub standing in for an answer — there
// is no answer involved. Same treatment as the haptics commands in kl_ovrp.c.
//
//   ovr_Log_NewEvent(name, ovrKeyValuePair *, count) — analytics. Telemetry
//     with no service behind it goes nowhere, and a headset that is not
//     attached to an Oculus account has nowhere to send it anyway.
//   ovr_Voip_SetMicrophoneMuted(ovrVoipMuteState) — the local mic gate for
//     in-game voice chat. RE4's OculusPlatform module pushes it during startup.
//     Nothing here captures audio at all — kl_aaudio refuses capture streams by
//     design and no microphone is presented — so there is no stream to mute and
//     nothing the guest can learn from the call. It is deliberately NOT filed
//     with the enumeration-answers-none group: those state an absence, and this
//     one is a command with no reply.
//   The rest of the outbound-only surface, added whole for pttr rather than one
//   launch at a time (see the derived-request note above for the method). Every
//   name here is a COMMAND with no reply: the guest hands us a setting, a
//   position, a callback or a buffer to free, and asks nothing back. There is no
//   voice chat here to configure, no NetSync session to place a listener in, no
//   packet we allocated to free, and no test platform to reset.
//
//   ovr_CrashApplication is in this group deliberately even though the enum has
//   a member for it: it is the SDK's deliberate-crash debug hook, and the one
//   thing it must NOT do here is what its name says.
static const char *const g_plat_drop[] = {
    "ovr_Log_NewEvent",
    "ovr_Voip_SetMicrophoneMuted",
    "ovr_Voip_Start", "ovr_Voip_Stop", "ovr_Voip_Accept",
    "ovr_Voip_SetOutputSampleRate", "ovr_Voip_SetNewConnectionOptions",
    "ovr_Voip_SetSystemVoipMicrophoneMuted", "ovr_Voip_SetSystemVoipPassthrough",
    "ovr_Voip_SetMicrophoneFilterCallback",
    "ovr_Voip_SetMicrophoneFilterCallbackWithFixedSizeBuffer",
    "ovr_Voip_DestroyEncoder", "ovr_Voip_DestroyDecoder",
    "ovr_Microphone_Destroy", "ovr_Microphone_Start", "ovr_Microphone_Stop",
    "ovr_Microphone_SetAcceptableRecordingDelayHint",
    "ovr_NetSync_SetListenerPosition", "ovr_Packet_Free",
    "ovr_ApplicationLifecycle_LogDeeplinkResult",
    "ovr_SetDeveloperAccessToken", "ovr_UnityResetTestPlatform",
    "ovr_CrashApplication",
};

static int plat_is_drop(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_drop / sizeof g_plat_drop[0]; i++)
        if (strcmp(g_plat_drop[i], name) == 0) return 1;
    return 0;
}

static const char g_options_cookie[] = "klepton-ovr-options";

static uint64_t klplat_options_create(const char *name) {
    plat_hit(name);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] %s -> opaque cookie (options objects are local "
                        "state; the request they feed cannot be made)\n", name);
    }
    return (uint64_t)(uintptr_t)g_options_cookie;
}

static int plat_is_options_create(const char *name) {
    return strstr(name, "Options_Create") != NULL;
}

// ---------------------------------------------------------------------------
// Launch details: HOW the app was started.
//
// Not a request — a plain accessor returning an opaque handle that the managed
// SDK immediately picks apart, reading a dozen ovr_LaunchDetails_* fields off it
// in `Models.LaunchDetails`'s constructor. SUPERHOT asks while building its main
// scene.
//
// The truthful answer is "normally, by the user, with nothing attached": no
// deeplink message, no destination, no room, no invited users, LaunchType 0
// (Unknown). That is precisely what every accessor answers below, because 0 is
// simultaneously the zero enum, the zero integer and the NULL pointer — and the
// SDK null-checks each optional list before wrapping it, so NULL is how it
// spells "there was none", not a hole.
//
// The handle itself must be NON-NULL for the same reason the options cookie is:
// it is carried straight into those accessors, and on a real platform a NULL
// there is a null dereference inside the library rather than an empty answer.
static const char g_launch_cookie[] = "klepton-launch-details";

static uint64_t klplat_launch_details(const char *name) {
    plat_hit(name);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] %s -> opaque handle: launched normally, by the "
                        "user, with nothing attached (no deeplink, no destination, "
                        "no invited users)\n", name);
    }
    return (uint64_t)(uintptr_t)g_launch_cookie;
}

static uint64_t klplat_launch_field(const char *name) {
    plat_hit(name);
    return 0;                 // the zero enum, the zero int and NULL, all at once
}

static int plat_is_launch_details(const char *name) {
    return strcmp(name, "ovr_ApplicationLifecycle_GetLaunchDetails") == 0;
}

static int plat_is_launch_field(const char *name) {
    return strncmp(name, "ovr_LaunchDetails_", 18) == 0;
}

// ...and the empty ARRAY that a NULL list handle turns into one call later.
//
// The managed `DeserializableList<T>` reads three things off a list handle —
// GetSize, GetElement and GetNextUrl — and this SDK version wraps the pointer
// WITHOUT null-checking it first, so answering ovr_LaunchDetails_GetUsers with
// NULL (correctly: nobody was invited) still ends in ovr_UserArray_GetSize(NULL)
// one call later. Size 0 is the same answer the NULL was: there is nobody in it.
//
// GetElement is deliberately NOT here. It can only be reached on an array with
// elements, which is an array we did not produce, so it keeps the fail-closed
// default and would stop the run by name rather than hand back a User the guest
// would then read fields off.
static int plat_is_empty_array(const char *name) {
    const char *p = strstr(name, "Array_Get");
    if (!p) return 0;
    return strcmp(p, "Array_GetSize") == 0 || strcmp(p, "Array_GetNextUrl") == 0;
}

static uint64_t klplat_empty_array(const char *name) {
    plat_hit(name);
    return 0;                 // no elements, and no next page to fetch them from
}

// The setters and the destructor. Both are genuinely nothing to do, not stubs
// standing in for something: there is no options state to keep.
static int plat_is_options_sink(const char *name) {
    return strstr(name, "Options_Destroy") != NULL ||
           strstr(name, "Options_Set") != NULL;
}

// ---------------------------------------------------------------------------
// Launch details: how the app was started. Not a request — a plain accessor
// returning an opaque handle the managed SDK immediately picks apart
// (Models.LaunchDetails reads a dozen ovr_LaunchDetails_* fields off it in its
// constructor). SUPERHOT asks while building its main scene.
//
// The truthful answer is "normally, by the user, with nothing attached": no
// deeplink message, no destination, no room, no invited users, LaunchType 0
// (Unknown). That is what every ovr_LaunchDetails_* accessor reports below —
// 0 for the integers and enums, NULL for the strings — and it is the same
// answer the real platform gives an app nobody deeplinked into.
//
// The handle itself must be non-NULL for the same reason the options cookie
// is: a NULL would be carried into those accessors, and on a real platform
// that is a null deref inside the library rather than an empty answer.
static uint64_t klplat_GetLaunchDetails(void) {
    plat_hit("ovr_ApplicationLifecycle_GetLaunchDetails");
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_ApplicationLifecycle_GetLaunchDetails -> empty "
                        "(launched normally: no deeplink, no destination, no party)\n");
    }
    return (uint64_t)(uintptr_t)g_launch_cookie;
}

// The accessors over it, plus the user array one of them hands back. Reporting
// "nothing" is the answer, not a stand-in for one: 0 is an empty string / a
// zero id / LaunchType_Unknown / an array of length 0, and the managed models
// are built to be told exactly that.
static uint64_t klplat_nothing(const char *name) {
    plat_hit(name);
    return 0;
}

// ovrLaunchType_ToString — the enum-to-string helper beside those accessors, and
// UE4's OculusPlatform module calls it on whatever GetLaunchType answered so it
// can log the launch. In this title's own libovrplatformloader.so (+0x86898) it
// is a loader THUNK — `str w0,[sp,#0xc]; ldr x8,[got+0x120]; ldr w0,[sp,#0xc];
// blr x8` — so the binary states the ABI (one 32-bit enum in w0, a pointer out)
// and nothing else; the table it forwards to lives in com.oculus.horizon, which
// is not here. The strings are the platform SDK's own OVR_LaunchType.h spelling.
//
// We answer 0 / ovrLaunchType_Unknown from every launch accessor (see above), so
// "Unknown" is the string this actually produces on this host. The rest of the
// table is there so the answer stays right if that ever changes, and the default
// is the SDK's own "UNKNOWN_ERROR" rather than NULL — a caller that logs the
// result with %s must not be handed one.
static const char *klplat_LaunchType_ToString(int v) {
    plat_hit("ovrLaunchType_ToString");
    switch (v) {
    case 0: return "Unknown";
    case 1: return "Normal";
    case 2: return "Invite";
    case 3: return "Coordinated";
    case 4: return "Deeplink";
    }
    return "UNKNOWN_ERROR";
}

static int plat_is_launch_accessor(const char *name) {
    return strncmp(name, "ovr_LaunchDetails_", 18) == 0 ||
           strncmp(name, "ovr_UserArray_", 14) == 0;
}

// ---------------------------------------------------------------------------
// Accessors over a message we did not produce, and the arrays hanging off one.
//
// Every request above answers 0, and a request that answered 0 queues no
// message — so on a Unity guest the SDK synthesizes the failure client-side and
// none of these is reachable. They are answered anyway, because "not reachable"
// is a property of the guest's SDK version rather than of this file, and the
// price of being wrong about it has been one launch per name.
//
// Zero is the right answer for all of them at once, and it is the answer the
// real SDK gives for a field that is not on an error message: an empty array, a
// zero id, a zero rank or score, and NULL for every string and sub-handle. The
// managed models are built to be told exactly that — DeserializableList<T>
// reads GetSize and stops, and every optional sub-object is null-checked.
//
// Narrow on purpose: the message / array / error / leaderboard-entry /
// challenge-entry families only. Everything else still falls through to the
// terminal tier, which now NAMES what it answered instead of stopping the run,
// so the next log is still a work list.
//
// It runs AFTER plat_is_request in kl_ovrplat_sym, so a request whose name
// happens to contain one of these fragments is still classified as a request.
static int plat_is_model_accessor(const char *name) {
    return strstr(name, "Array_Get") != NULL              ||
           strncmp(name, "ovr_Error_", 10)            == 0 ||
           strncmp(name, "ovr_Message_Get", 15)       == 0 ||
           strncmp(name, "ovr_LeaderboardEntry_", 21) == 0 ||
           strncmp(name, "ovr_LeaderboardUpdateStatus_", 28) == 0 ||
           strncmp(name, "ovr_ChallengeEntry_", 19)    == 0 ||
           strncmp(name, "ovr_Challenge_", 14)         == 0;
}

static uint64_t klplat_model_accessor(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1)
        fprintf(stderr, "  [plat] %s -> 0 (accessor over a message this host never "
                        "produced: an empty array, a zero id and a NULL string, "
                        "which is what the SDK reads off an error message)\n", name);
    return 0;
}

// The three handle-returning CREATORS outside the options family, which are the
// only names in this guest's 700 where a NULL is a contract violation rather
// than an empty answer — and the only ones where NULL is nonetheless the honest
// report, because the device really is absent.
//
// kl_aaudio refuses capture streams by design and no microphone is presented on
// this host, so ovr_Microphone_Create has nothing to open; the two Voip codec
// creators belong to a voice session that cannot exist without it. A real
// headset with no microphone permission answers exactly this way, and the Unity
// SDK's Voip.CreateEncoder / Microphone wrappers null-check what they get back.
//
// ovr_GetLoggedInUserLocale rides along because it is the same shape — a
// synchronous accessor returning a const char * we do not have. NULL is the
// SDK's "unknown", and Marshal.PtrToStringAnsi(NULL) is null on the managed
// side rather than a fault.
static const char *const g_plat_absent[] = {
    "ovr_Microphone_Create", "ovr_Voip_CreateEncoder", "ovr_Voip_CreateDecoder",
    "ovr_GetLoggedInUserLocale",
};

static int plat_is_absent(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_absent / sizeof g_plat_absent[0]; i++)
        if (strcmp(g_plat_absent[i], name) == 0) return 1;
    return 0;
}

static uint64_t klplat_absent(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1)
        fprintf(stderr, "  [plat] %s -> NULL (the device or datum it would hand "
                        "back is genuinely absent on this host, which is what a "
                        "headset with no microphone and no session reports)\n", name);
    return 0;
}

static int plat_is_request(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_request / sizeof g_plat_request[0]; i++)
        if (strcmp(g_plat_request[i], name) == 0) return 1;
    for (size_t i = 0; i < sizeof g_plat_request_prefix / sizeof g_plat_request_prefix[0]; i++)
        if (strncmp(name, g_plat_request_prefix[i],
                    strlen(g_plat_request_prefix[i])) == 0) return 1;
    return 0;
}

// Who is signed in — the C SDK's synchronous accessor, not a request. It
// returns an `ovrID`, and 0 is that type's own "no such user": the managed side
// spells the same thing `User == null`, and every caller of it null-checks,
// because a user really can be absent on a real headset (offline, or before the
// platform has initialized).
//
// Not an ownership question and not a request, so neither classifier is right
// for it: `ovr_User_GetLoggedInUser` beside them is the ASYNC form and correctly
// answers "the request could not be made", while this one is asked and answered
// on the spot. RE4 asks during engine init.
//
// `KL_PLAT_USER=<ovrID>` answers an id instead, and it exists because 0 is not
// free for every guest: UE4's OnlineSubsystemOculus REFUSES to Init without a
// logged-in user, and a subsystem that did not Init never Ticks — so it never
// calls ovr_PopMessage, and every completion this file queues (the entitlement
// answer included) sits in the queue forever. Measured on Tetris Effect r2:
// 0 gives a platform surface of 9 calls and no pump at all; a non-zero id gives
// 15, `ovr_PopMessage` x8697, and the entitlement completion actually popped,
// checked with ovr_Message_IsError and freed.
//
// It is OFF by default and stays off. Answering an id is a claim about a person
// where the truthful answer is "nobody", so it has to be asked for out loud —
// a target that needs one bakes it, the same shape as PORTING.md's KL_PLAT_INIT
// bake, and the log says so on every run that uses it.
//
// ...and on a KL_PLAT_INIT target, where the platform reported itself up, a
// user has to exist behind that report: "initialized but nobody home" is
// exactly what deadpool's boot refuses to run — the ForceQuit lands right
// after this answer. The id served is the declared local user's (see the
// constants above): not discovered, issued.
static uint64_t klplat_GetLoggedInUserID(void) {
    plat_hit("ovr_GetLoggedInUserID");
    if (klplat_init_target()) {
        static int said_id;
        if (!said_id) {
            said_id = 1;
            fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> 0x%llx (the "
                            "declared local user — KL_PLAT_INIT reported the "
                            "platform up, so somebody is home)\n",
                    (unsigned long long)KLPLAT_USER_ID);
        }
        return KLPLAT_USER_ID;
    }
    // KL_PLAT_USER has two spellings. `=<ovrID>` (anything above 1) answers that
    // id; `=1` (or on/yes, or the per-target default from klplat_user_default(),
    // the SAME switch klplat_User_GetLoggedInUser reads for the async form) is
    // the synthetic OFFLINE user, and answers the same ovrID ovr_User_GetID hands
    // back, so the sync and async views of "who is playing" agree. Some titles
    // gate boot on "is anyone signed in?" and ForceQuit or fatal when this
    // returns 0 (Asgard's Wrath 2, Into the Radius, Red Matter 2: "Unable to get
    // a valid UserID"). This is a session identity, NOT an ownership answer:
    // entitlement stays a separate request and grants nothing here.
    const char *forced = kl_env_str("KL_PLAT_USER", NULL);
    unsigned long long id = forced && *forced ? strtoull(forced, NULL, 0) : 0;
    if (id == 1 || (!id && kl_env_on("KL_PLAT_USER", klplat_user_default())))
        id = 1000000000000042ull;   // == klplat_User_GetID's synthetic ovrID
    static int said;
    if (!said) {
        said = 1;
        if (id)
            fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> %llu (KL_PLAT_USER: "
                            "there is no Oculus account here, this id is INVENTED "
                            "— set so the guest's platform subsystem will "
                            "initialize and pump its messages; NOT an entitlement "
                            "grant)\n", id);
        else
            fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> 0 (no platform user; "
                            "there is no service here to be signed in to; set "
                            "KL_PLAT_USER=1 for a synthetic offline user)\n");
    }
    return id;
}

// ovr_GetLoggedInUserLocale — a SYNCHRONOUS getter (returns const char*, not an
// ovrRequest), the locale the store would report for the signed-in user. There
// is no platform user here, but the guest reads the string to pick a language,
// and a NULL would crash the read — so answer the host's, defaulting to en_US.
static uint64_t klplat_GetLoggedInUserLocale(void) {
    plat_hit("ovr_GetLoggedInUserLocale");
    static char loc[16];
    if (!loc[0]) {
        const char *lang = getenv("LANG");            // e.g. "en_US.UTF-8"
        int i = 0;
        if (lang) for (; lang[i] && lang[i] != '.' && i < (int)sizeof loc - 1; i++) loc[i] = lang[i];
        if (i == 0) { loc[0]='e'; loc[1]='n'; loc[2]='_'; loc[3]='U'; loc[4]='S'; i=5; }
        loc[i] = '\0';
    }
    return (uint64_t)(uintptr_t)loc;
}

// The KL_PLAT_INIT declared-user surface (consulted ahead of the KL_PLAT_USER
// one above, and only on KL_PLAT_INIT targets — see g_plat_user_impl).
// The ASYNC form of the same question, a REQUEST returning `Request<User>`.
// On a KL_PLAT_INIT target it gets the real treatment — a typed
// User_GetLoggedInUser completion through the pump, whose payload the three
// accessors below read — because UE4's identity login awaits exactly that
// message and 0 would park it on a completion that cannot come. Everywhere
// else the truthful answer stands: 0, "the request could not be made" (Beat
// Saber 1.28 takes its offline path from there, ~swap 34k of the boot).
static uint64_t klplat_init_User_GetLoggedInUser(void) {
    plat_hit("ovr_User_GetLoggedInUser");
    return klplat_request("ovr_User_GetLoggedInUser",
                          KLPLAT_MSG_INIT_LOGGED_IN_USER, 0);
}

// The User payload accessors. The payload handle IS the message (same move as
// klplat_Message_GetPayload), and the two fields are the declared constants:
// there is exactly one user on this host, so no message carries a different
// answer. ovr_User_GetID's return is an ovrID, ovr_User_GetOculusID's a
// string the SDK copies.
static uint64_t klplat_init_User_GetID(const void *h) {
    plat_hit("ovr_User_GetID");
    (void)h;
    return KLPLAT_USER_ID;
}
static const char *klplat_init_User_GetOculusID(const void *h) {
    plat_hit("ovr_User_GetOculusID");
    (void)h;
    return KLPLAT_USER_NAME;
}

// The third leg of UE4's identity login (FOculusIdentityInterface asks for
// the token, the user, and then combines them). A `Request<string>`, so the
// answer rides the string-payload channel the cloud directory already uses;
// the value is the declared placeholder above, on KL_PLAT_INIT targets only.
static uint64_t klplat_init_User_GetAccessToken(void) {
    plat_hit("ovr_User_GetAccessToken");
    return klplat_request_str("ovr_User_GetAccessToken",
                              KLPLAT_MSG_INIT_ACCESS_TOKEN, 0, KLPLAT_USER_TOKEN);
}

// The real libovrplatformloader exports JNI_OnLoad and caches the JavaVM out of
// it. Ours has no JNI surface to set up, so the whole body is the version
// number Android checks for.
//
// Reached only because System.load() now honours the Android contract and calls
// JNI_OnLoad on what it loaded (kl_jni.c). Before that, this library was
// dlopen'd and never initialized, and nothing noticed — which is exactly the
// bug that change fixes, so this is the cost of fixing it rather than a
// workaround. Answering the version is not a stub: a library that returns an
// unrecognised version is one that REFUSED to initialize, and Android unloads it.
//
// Below plat_is_drm in kl_ovrplat_sym, like everything in this table, so the
// entitlement classifier still runs first and this cannot be a way around it.
static uint64_t klplat_JNI_OnLoad(void *vm, void *reserved) {
    (void)vm; (void)reserved;
    plat_hit("JNI_OnLoad");
    return 0x00010006;                 // JNI_VERSION_1_6
}

// ---------------------------------------------------------------------------
// The completion queue: a request builds a real message, queues it and returns
// a request id; the SDK's own pump (ovr_PopMessage, already called every frame)
// delivers it on a later call. Every string points into the message and is
// freed by ovr_FreeMessage, after the SDK has copied what it needs — that is
// the real platform's lifetime contract.

// One queue, two message shapes. Publishing is its own function because the
// moment a message is on the queue it belongs to whoever pops it next — reading
// anything back out of `m` after this returns is a use-after-free waiting for a
// second thread — so the request id comes back by value and the caller keeps
// nothing.
static uint64_t klplat_enqueue(klplat_msg *m) {
    pthread_mutex_lock(&g_msg_mu);
    uint64_t id = m->request_id = g_next_request++;
    if (g_msg_tail) g_msg_tail->next = m;
    else            g_msg_head = m;
    g_msg_tail = m;
    pthread_mutex_unlock(&g_msg_mu);
    return id;
}

// A completion with no payload: type, request id, and "not an error", which is
// every field the SDK reads off a message it has no accessor for. The
// entitlement answer is exactly this shape.
static uint64_t klplat_queue_bare(uint32_t type) {
    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = type;
    return klplat_enqueue(m);
}

// ---------------------------------------------------------------------------
// ovr_CloudStorage2_GetUserDirectoryPath — the one cloud-save call that has to
// be ANSWERED rather than refused.
//
// It was in the request-returning families below, answering 0 ("the request
// could not be made"), on the reasoning that a game which cannot reach the
// cloud writes locally. For SUPERHOT that reasoning is wrong in a way that
// cost this port a week: 0 does not put the game on a local path, it puts it
// on NO path. `VRSaveManager::LoadAsync` (libil2cpp+0xd2174c) asks for the
// directory and hangs its whole continuation off the answer —
//
//     req = CloudStorage2.GetUserDirectoryPath();
//     if (req == null) FailedToLoad(...);            // quits
//     req.OnComplete(LoadAsyncContinued);            // the ONLY way forward
//
// — and the SDK's OnComplete drops a request whose id is 0 on the floor with
// "An unknown error occurred. Request failed.", so `LoadAsyncContinued` never
// runs, `VRLaunch::OnLoadCompleted` never runs, and the `LoadSceneAsync` at
// the end of it never happens. The launch scene stays on screen forever. That
// is the same shape as the entitlement bug above: a request id nothing would
// ever complete.
//
// So: queue a real completion carrying a real directory. This asserts nothing
// about ownership — a per-user save directory is the user's own data, not a
// purchase — and the honest answer on a machine with no Oculus cloud is a
// local directory that is really there. `LoadAsyncContinued` stores it as the
// save folder and `PerformOneTimeSaveMerging` calls IsDirectoryWritable on it
// before anything else (and quits if it is not), so it is created here.
//
// 0x76a42eee is ovrMessageType_CloudStorage2_GetUserDirectoryPath, read out of
// the guest's own metadata with KL_PROBE_ENUM=Oculus.Platform.Message/
// MessageType (kl_mprobe.c) — the same table, cross-checked in the same run
// against the two constants already baked here: Entitlement_GetIsViewerEntitled
// 0x186b58b1 and AssetFile_GetList 0x4afc6f74, both exact.
#define KL_MSG_CLOUDSTORAGE2_GETUSERDIRECTORYPATH 0x76a42eeeu

static uint64_t klplat_CloudStorage2_GetUserDirectoryPath(void) {
    plat_hit("ovr_CloudStorage2_GetUserDirectoryPath");

    // KL_PLAT_CLOUD_DIR overrides the directory; setting it EMPTY restores the
    // old request-id-0 refusal, which is the A/B for "is this what unblocked
    // the launch".
    const char *over = kl_env_str("KL_PLAT_CLOUD_DIR", NULL);
    if (over && !*over) return 0;

    static char dir[1100];
    if (!dir[0]) {
        if (over) snprintf(dir, sizeof dir, "%s", over);
        // A sibling of getFilesDir()'s `files`, not a child: the game merges
        // this directory against Application.persistentDataPath (which IS
        // `files`), and a save folder nested inside the folder it is being
        // merged with invites the game to find its own copies.
        else snprintf(dir, sizeof dir, "%s/cloudstorage", kl_jni_files_dir());
    }
    struct stat st;
    if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0) {
        fprintf(stderr, "  [plat] ovr_CloudStorage2_GetUserDirectoryPath: cannot "
                        "create %s — answering 0 (the request could not be made)\n", dir);
        return 0;
    }

    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = KL_MSG_CLOUDSTORAGE2_GETUSERDIRECTORYPATH;
    m->str  = strdup(dir);
    if (!m->str) { free(m); return 0; }
    uint64_t req = klplat_enqueue(m);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_CloudStorage2_GetUserDirectoryPath -> \"%s\" "
                        "(no Oculus cloud here; the user's save directory is local)\n", dir);
    }
    return req;
}

// The message pump. NULL still means "no messages queued" — the documented
// steady state; it can also hand out a queued completion.
static void *klplat_PopMessage(void) {
    plat_hit("ovr_PopMessage");
    pthread_mutex_lock(&g_msg_mu);
    klplat_msg *m = g_msg_head;
    if (m) {
        g_msg_head = m->next;
        if (!g_msg_head) g_msg_tail = NULL;
        m->next = NULL;
    }
    pthread_mutex_unlock(&g_msg_mu);
    return m;
}

static uint64_t klplat_FreeMessage(klplat_msg *m) {
    plat_hit("ovr_FreeMessage");
    if (!m) return 0;
    for (size_t i = 0; i < m->count; i++) free(m->items[i].filepath);
    free(m->items);
    free(m->str);
    free(m);
    return 0;
}

// Accessors over the message and its contents. The handles are our own
// structs; every read goes through one of these, so the layout is ours.
static uint64_t    klplat_msg_type(klplat_msg *m)        { return m ? m->type : 0; }
static uint64_t    klplat_msg_is_error(klplat_msg *m)    { return m && m->is_error; }
static uint64_t    klplat_msg_request_id(klplat_msg *m)  { return m ? m->request_id : 0; }
static void       *klplat_msg_error(klplat_msg *m)       { return (m && m->is_error) ? m : NULL; }
static const char *klplat_err_message(klplat_msg *m)     { return m ? m->err : NULL; }
static uint64_t    klplat_err_code(klplat_msg *m)        { (void)m; return 1; }
static uint64_t    klplat_err_http(klplat_msg *m)        { (void)m; return 0; }
static void       *klplat_msg_string(klplat_msg *m)      { return m ? m->str : NULL; }
static void       *klplat_msg_native(klplat_msg *m)      { return m; }
static void       *klplat_msg_asset_array(klplat_msg *m) { return m; }
static uint64_t    klplat_arr_size(klplat_msg *m)        { return m ? m->count : 0; }
static void       *klplat_arr_element(klplat_msg *m, uint64_t i) {
    return (m && i < m->count) ? &m->items[i] : NULL;
}
// ---------------------------------------------------------------------------
// ovr_AssetFile_Status / _StatusById / _StatusByName — answered "available /
// not-entitled", for every name, on every target (see the carve-out comment at
// the top). The SDK's Message<AssetDetails>: one AssetDetails read through
// ovr_Message_GetAssetDetails. The three message types are the SDK's own
// ovrMessageType members (read out of a guest's metadata with KL_PROBE_ENUM,
// with AssetFile_GetList 0x4afc6f74 reproduced as the calibration): the
// managed side switches on the type before it reads anything, so a guessed
// number would be a completion that never arrives.
//
// What a caller sees is "this asset exists in the store and is not here and
// not yours": DownloadStatus "available", IapStatus "not-entitled", no file
// path. A game that would go on to download it hits ovr_AssetFile_Download*,
// which still aborts. Nothing here looks at the filesystem.
#define KL_MSG_ASSETFILE_STATUS       0x02d32f60u
#define KL_MSG_ASSETFILE_STATUSBYID   0x5d955d38u
#define KL_MSG_ASSETFILE_STATUSBYNAME 0x41cfda50u

// A stable nonzero id per name (FNV-1a over the basename), so a later *ById
// call about the same asset is answered about the same asset.
static uint64_t klplat_asset_id(const char *name) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)name; p && *p; p++)
        h = (h ^ *p) * 1099511628211ull;
    return h ? h : 1;
}

static uint64_t klplat_status_msg(const char *what, uint32_t type,
                                  const char *name, uint64_t id) {
    plat_hit(what);
    const char *base = name;
    if (base) { const char *sl = strrchr(base, '/'); if (sl) base = sl + 1; }
    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type  = type;
    m->items = calloc(1, sizeof *m->items);
    if (!m->items) { free(m); return 0; }
    m->count = 1;
    m->items[0].asset_id = id ? id : klplat_asset_id(base && *base ? base : "?");
    m->items[0].filepath = NULL;              // not here: no path to report
    uint64_t req = klplat_enqueue(m);         // `m` is the queue's from here on
    static int said;
    if (said++ < 8)
        fprintf(stderr, "  [plat] %s(\"%s\") -> available / not-entitled (no "
                        "platform here; nothing is owned, nothing is installed)\n",
                what, base && *base ? base : "<by id>");
    return req;
}
static uint64_t klplat_StatusByName(const char *name) {
    return klplat_status_msg("ovr_AssetFile_StatusByName",
                             KL_MSG_ASSETFILE_STATUSBYNAME, name, 0);
}
static uint64_t klplat_StatusById(uint64_t id) {
    return klplat_status_msg("ovr_AssetFile_StatusById",
                             KL_MSG_ASSETFILE_STATUSBYID, NULL, id);
}
static uint64_t klplat_Status(uint64_t id) {          // the deprecated spelling
    return klplat_status_msg("ovr_AssetFile_Status",
                             KL_MSG_ASSETFILE_STATUS, NULL, id);
}

// The single-AssetDetails payload, and the accessors over it.
static void       *klplat_msg_asset_details(klplat_msg *m) {
    return (m && m->count) ? &m->items[0] : NULL;
}
static uint64_t    klplat_ad_id(klplat_asset *a)       { return a ? a->asset_id : 0; }
static const char *klplat_ad_type(klplat_asset *a)     { (void)a; return "default"; }
static const char *klplat_ad_status(klplat_asset *a)   { (void)a; return "available"; }
static const char *klplat_ad_path(klplat_asset *a)     { (void)a; return NULL; }
static const char *klplat_ad_iap(klplat_asset *a)      { (void)a; return "not-entitled"; }
static void       *klplat_ad_language(klplat_asset *a) { (void)a; return NULL; }
static const char *klplat_ad_meta(klplat_asset *a)     { (void)a; return ""; }
// The language-pack sub-object: none, so its accessors answer NULL (the
// managed model reads them unconditionally when it parses an AssetDetails).
static const char *klplat_lang_string(void *h)         { (void)h; return NULL; }

// ---------------------------------------------------------------------------
// ovr_IAP_GetViewerPurchases — answered EMPTY.
//
// The one ownership question in this file that is answered with a NEGATIVE
// rather than refused. See the carve-out comment at the top for the policy; the
// mechanics are: queue a completion carrying a purchase array of length 0, hand
// the guest a real request id, and let its own pump deliver it. count is 0 and
// items is NULL, so every accessor over it already says "nothing".
//
// THE MESSAGE TYPE IS 0 (ovrMessage_Unknown), on purpose, and it is safe here
// for the same reason it is on the pinballfx branch (see the note under
// g_plat_request_prefix): UNREAL dispatches a completion on the REQUEST ID.
// FOnlineSubsystemOculus::OnReceiveMessage looks the id up in its
// RequestDelegates map and only falls back to ovr_Message_GetType for
// notifications, which have no id. Into the Radius is a UE 4.27 guest and
// libUE4 imports ovr_Message_GetRequestID / _IsError / _GetPurchaseArray for
// exactly that path. A UNITY guest would need the real ovrMessageType constant
// read out of its own metadata with KL_PROBE_ENUM (Messages.ParseMessageHandle
// switches on the type FIRST), so if this is ever wanted on a Unity title, that
// is the thing to add before it is used there — not a guessed number.
static uint64_t klplat_IAP_GetViewerPurchases(void) {
    plat_hit("ovr_IAP_GetViewerPurchases");

    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type  = 0;                 // ovrMessage_Unknown; UE dispatches on the id
    m->count = 0;                 // ...and the array is EMPTY. That is the answer.
    m->items = NULL;
    uint64_t req = klplat_enqueue(m);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_IAP_GetViewerPurchases -> request %llu, "
                        "completion queued with an EMPTY purchase array (this "
                        "viewer owns nothing: there is no Oculus Store account "
                        "here and no purchase history. Nothing is claimed, no "
                        "SKU is named, and every purchasing / delivery call "
                        "still refuses)\n", (unsigned long long)req);
    }
    return req;
}

// The accessors over that empty array. The message IS the array handle, as it
// is for the asset enumeration; size and element route to the shared array
// accessors, which answer 0 and NULL for count 0.
static uint64_t    klplat_purch_has_next(klplat_msg *m) { (void)m; return 0; }
static const char *klplat_purch_next_url(klplat_msg *m) { (void)m; return NULL; }

static const struct { const char *name; void *fn; } g_plat_enum_impl[] = {
    {"ovr_AssetFile_Status",                        klplat_Status},
    {"ovr_AssetFile_StatusById",                    klplat_StatusById},
    {"ovr_AssetFile_StatusByName",                  klplat_StatusByName},
    {"ovr_Message_GetAssetDetails",                 klplat_msg_asset_details},
    {"ovr_AssetDetails_GetAssetId",                 klplat_ad_id},
    {"ovr_AssetDetails_GetAssetType",               klplat_ad_type},
    {"ovr_AssetDetails_GetAssetType_Native",        klplat_ad_type},
    {"ovr_AssetDetails_GetDownloadStatus",          klplat_ad_status},
    {"ovr_AssetDetails_GetDownloadStatus_Native",   klplat_ad_status},
    {"ovr_AssetDetails_GetFilepath",                klplat_ad_path},
    {"ovr_AssetDetails_GetFilepath_Native",         klplat_ad_path},
    {"ovr_AssetDetails_GetIapStatus",               klplat_ad_iap},
    {"ovr_AssetDetails_GetIapStatus_Native",        klplat_ad_iap},
    {"ovr_AssetDetails_GetLanguage",                klplat_ad_language},
    {"ovr_AssetDetails_GetMetadata",                klplat_ad_meta},
    {"ovr_AssetDetails_GetMetadata_Native",         klplat_ad_meta},
    {"ovr_LanguagePackInfo_GetEnglishName",            klplat_lang_string},
    {"ovr_LanguagePackInfo_GetEnglishName_Native",     klplat_lang_string},
    {"ovr_LanguagePackInfo_GetNativeName",             klplat_lang_string},
    {"ovr_LanguagePackInfo_GetNativeName_Native",      klplat_lang_string},
    {"ovr_LanguagePackInfo_GetTag",                    klplat_lang_string},
    {"ovr_LanguagePackInfo_GetTag_Native",             klplat_lang_string},
    {"ovr_FreeMessage",                             klplat_FreeMessage},
    {"ovr_Message_GetType",                         klplat_msg_type},
    {"ovr_Message_IsError",                         klplat_msg_is_error},
    {"ovr_Message_GetRequestID",                    klplat_msg_request_id},
    {"ovr_Message_GetError",                        klplat_msg_error},
    {"ovr_Error_GetMessage",                        klplat_err_message},
    {"ovr_Error_GetMessage_Native",                 klplat_err_message},
    {"ovr_Error_GetDisplayableMessage",             klplat_err_message},
    {"ovr_Error_GetDisplayableMessage_Native",      klplat_err_message},
    {"ovr_Error_GetCode",                           klplat_err_code},
    {"ovr_Error_GetHttpCode",                       klplat_err_http},
    {"ovr_Message_GetString",                       klplat_msg_string},
    {"ovr_Message_GetString_Native",                klplat_msg_string},
    {"ovr_Message_GetNativeMessage",                klplat_msg_native},
    // The empty purchase answer. ovr_Purchase_* is NOT here on purpose:
    // an empty array has no element to read a SKU off, so those names keep the
    // refusal.
    {"ovr_IAP_GetViewerPurchases",                  klplat_IAP_GetViewerPurchases},
    {"ovr_Message_GetPurchaseArray",                klplat_msg_asset_array},
    {"ovr_PurchaseArray_GetSize",                   klplat_arr_size},
    {"ovr_PurchaseArray_GetElement",                klplat_arr_element},
    {"ovr_PurchaseArray_HasNextPage",               klplat_purch_has_next},
    {"ovr_PurchaseArray_GetNextUrl",                klplat_purch_next_url},
};

// ---------------------------------------------------------------------------
// ovrKeyValuePair_makeString — the platform SDK's own struct constructor,
// which libUE4 (Deadpool) LINKS rather than reaching through dlsym.
//
// The ABI is the header's (OVR_Platform_Defs.h, the 1.x-era SDK this loader
// speaks): a 32-byte struct, returned BY VALUE — which is an sret through x8
// at this size, so this stays a plain C struct return and lets the compiler
// do the ABI. The real loader's copy is a gate, not an implementation: it
// aborts unless ovr_PlatformInitializeAndroid ran (verified in the shipped
// binary — "...makeString was called before ovr_PlatformInitializeAndroid!"),
// because the pair only matters to calls the service answers. Here it is pure
// local state, so the constructor just constructs.
typedef struct {
    const char *key;
    int type;                        // 0 string, 1 int, 2 double (the header's enum)
    int int_value;
    double double_value;
    const char *string_value;
} klplat_kvpair;

static klplat_kvpair klplat_KeyValuePair_makeString(const char *key, const char *value) {
    plat_hit("ovrKeyValuePair_makeString");
    klplat_kvpair p = { key, 0, 0, 0.0, value };
    return p;
}

static const struct { const char *name; void *fn; } g_plat_impl[] = {
    {"JNI_OnLoad",                (void *)klplat_JNI_OnLoad},
    {"ovr_IsPlatformInitialized", (void *)klplat_IsPlatformInitialized},
    {"ovr_GetLoggedInUserID",     (void *)klplat_GetLoggedInUserID},
    {"ovr_GetLoggedInUserLocale", (void *)klplat_GetLoggedInUserLocale},
    {"ovr_UnityInitWrapper",      (void *)klplat_UnityInitWrapper},
    {"ovr_PopMessage",            (void *)klplat_PopMessage},
    {"ovr_FreeMessage",           (void *)klplat_FreeMessage},
    {"ovr_Entitlement_GetIsViewerEntitled", (void *)klplat_Entitlement_GetIsViewerEntitled},
    {"ovr_UserAgeCategory_Get",   (void *)klplat_UserAgeCategory_Get},
    {"ovr_Application_GetVersion",(void *)klplat_Application_GetVersion},
    {"ovr_Message_GetApplicationVersion",      (void *)klplat_Message_GetPayload},
    {"ovr_AssetFile_GetList",                  (void *)klplat_AssetFile_GetList},
    {"ovr_Message_GetAssetFileDeliveryList",   (void *)klplat_Message_GetPayload},
    {"ovr_AssetFileDeliveryList_GetSize",      (void *)klplat_AssetFileDeliveryList_GetSize},
    // ...and the names this SDK build actually reads the same payload through
    // (measured: the GetList completion was consumed via GetAssetDetailsArray,
    // which the DRM family then refused by prefix). An EMPTY details array
    // names no content, delivers no content, and unblocks the reader.
    {"ovr_Message_GetAssetDetailsArray",       (void *)klplat_Message_GetPayload},
    {"ovr_AssetDetailsArray_GetSize",          (void *)klplat_AssetFileDeliveryList_GetSize},
    // The user access token and the achievement catalogue — two request-style
    // calls the game issues during platform init and gates its menu on. Both
    // complete with benign, empty/offline data (see the handlers above) rather
    // than the bare 0 that lands the guest on its "Request failed" path. The
    // exact-match binding here overrides the ovr_Achievements_ prefix rule.
    {"ovr_User_GetAccessToken",                (void *)klplat_User_GetAccessToken},
    {"ovr_Achievements_GetAllDefinitions",     (void *)klplat_Achievements_GetAllDefinitions},
    {"ovr_Message_GetAchievementDefinitionArray", (void *)klplat_Message_GetPayload},
    // The logged-in user (async) and the User accessors its completion feeds.
    // Gated by KL_PLAT_USER inside the handler; the accessors are inert unless a
    // user was handed out. The exact-match binding overrides ovr_User_GetLoggedInUser's
    // entry in g_plat_request.
    {"ovr_User_GetLoggedInUser",               (void *)klplat_User_GetLoggedInUser},
    {"ovr_Message_GetUser",                    (void *)klplat_Message_GetPayload},
    {"ovr_User_GetID",                         (void *)klplat_User_GetID},
    {"ovr_User_GetOculusID",                   (void *)klplat_User_GetOculusID},
    {"ovr_User_GetDisplayName",                (void *)klplat_User_GetDisplayName},
    {"ovr_ApplicationVersion_GetCurrentCode",  (void *)klplat_AppVersion_code},
    {"ovr_ApplicationVersion_GetLatestCode",   (void *)klplat_AppVersion_code},
    {"ovr_ApplicationVersion_GetCurrentName",  (void *)klplat_AppVersion_name},
    {"ovr_ApplicationVersion_GetLatestName",   (void *)klplat_AppVersion_name},
    {"ovr_ApplicationVersion_GetReleaseDate",  (void *)klplat_AppVersion_unknown},
    {"ovr_ApplicationVersion_GetSize",         (void *)klplat_AppVersion_unknown},
    // The message accessors. Each takes the handle in x0, so they cannot go
    // through a named trampoline — see the note above klplat_Message_GetType.
    {"ovr_Message_GetType",       (void *)klplat_Message_GetType},
    // "The native message" is this message — the SDK keeps the handle around to
    // hand back to callers that want the raw pointer, and there is nothing
    // behind ours that it is not already holding.
    {"ovr_Message_GetNativeMessage", (void *)klplat_Message_GetPayload},
    {"ovr_Message_IsError",       (void *)klplat_Message_IsError},
    {"ovr_Message_GetRequestID",  (void *)klplat_Message_GetRequestID},
    // A failed request's completion, and the accessors a guest reads off it.
    // libUnreal imports ovr_Message_GetError and ovr_Error_GetMessage by name
    // (FOnlineSubsystemOculus::OnReceiveMessage logs the error and then retires
    // the delegate); the rest are here so that a guest which logs more of the
    // error does not trade round 16's hang for an unimplemented-name abort.
    {"ovr_Message_GetError",      (void *)klplat_Message_GetError},
    {"ovr_Error_GetMessage",      (void *)klplat_Error_GetMessage},
    {"ovr_Error_GetMessage_Native", (void *)klplat_Error_GetMessage},
    {"ovr_Error_GetDisplayableMessage", (void *)klplat_Error_GetDisplayableMessage},
    {"ovr_Error_GetDisplayableMessage_Native", (void *)klplat_Error_GetDisplayableMessage},
    {"ovr_Error_GetCode",         (void *)klplat_Error_GetCode},
    {"ovr_Error_GetHttpCode",     (void *)klplat_Error_GetHttpCode},
    {"ovr_Message_GetPlatformInitialize",      (void *)klplat_Message_GetPayload},
    {"ovr_PlatformInitialize_GetResult",       (void *)klplat_payload_int},
    {"ovr_Message_GetUserAccountAgeCategory",  (void *)klplat_Message_GetPayload},
    {"ovr_UserAccountAgeCategory_GetAgeCategory", (void *)klplat_payload_int},
    // The string payload, and the one request that carries one.
    {"ovr_Message_GetString",     (void *)klplat_Message_GetString},
    {"ovr_CloudStorage2_GetUserDirectoryPath",
                                  (void *)klplat_CloudStorage2_GetUserDirectoryPath},
    {"ovr_ApplicationLifecycle_GetLaunchDetails", (void *)klplat_GetLaunchDetails},
    // Takes the enum value in w0, so it cannot go through a named trampoline
    // either — see klplat_LaunchType_ToString.
    {"ovrLaunchType_ToString",    (void *)klplat_LaunchType_ToString},
    // The SDK's own struct constructor, which libUE4 (deadpool) links.
    {"ovrKeyValuePair_makeString",  (void *)klplat_KeyValuePair_makeString},
};
static const char g_plat_handle[] = "klepton-ovrplatformloader";

int kl_ovrplat_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libovrplatformloader.so") == 0 ||
           strcmp(b, "ovrplatformloader") == 0;
}

void *kl_ovrplat_dlopen(const char *soname) {
    if (!kl_ovrplat_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    fprintf(stderr, "  [plat] guest dlopen(\"%s\") -> synthetic Oculus Platform "
                    "(the real one forwards to com.oculus.horizon, which is not "
                    "here; see kl_ovrplat.h)\n", b);
    return (void *)g_plat_handle;
}

// The declared local user's surface (KL_PLAT_INIT targets ONLY): the request,
// the payload accessor and the User field accessors it lands on. A table of its
// own, consulted only when the knob is on, so every other target keeps driver's
// classification of these names (request-fails / model accessor) unchanged.
static const struct { const char *name; void *fn; } g_plat_user_impl[] = {
    {"ovr_User_GetLoggedInUser",    (void *)klplat_init_User_GetLoggedInUser},
    {"ovr_Message_GetUser",         (void *)klplat_Message_GetPayload},
    {"ovr_User_GetID",              (void *)klplat_init_User_GetID},
    {"ovr_User_GetOculusID",        (void *)klplat_init_User_GetOculusID},
    {"ovr_User_GetAccessToken",     (void *)klplat_init_User_GetAccessToken},
};

int kl_ovrplat_is_handle(const void *h) { return h == (const void *)g_plat_handle; }

void *kl_ovrplat_sym(const char *name) {
    if (!name) return NULL;
    int s = plat_slot(name);                    // resolved, whatever happens next

    // Classified at *resolve* time as well as call time, so kl_ovrplat_report can
    // show what the guest went looking for even on a run where nothing aborted.
    // Resolving is still only a measurement — the refusal is at the call.
    if (plat_is_drm(name)) {
        if (s >= 0) g_plat[s].drm = 1;
        return kl_named_stub(name, (void *)klplat_drm);
    }
    // The ownership families' request half: answered 0 rather than refused.
    // Checked here, immediately after the refusal, so the two halves of the
    // policy read together — requests fail honestly, accessors still refuse.
    if (plat_is_drm_request(name))
        return kl_named_stub(name, (void *)klplat_drm_request);
    if (klplat_init_target())
        for (size_t i = 0; i < sizeof g_plat_user_impl / sizeof g_plat_user_impl[0]; i++)
            if (strcmp(g_plat_user_impl[i].name, name) == 0) return g_plat_user_impl[i].fn;
    for (size_t i = 0; i < sizeof g_plat_impl / sizeof g_plat_impl[0]; i++)
        if (strcmp(g_plat_impl[i].name, name) == 0) return g_plat_impl[i].fn;
    for (size_t i = 0; i < sizeof g_plat_enum_impl / sizeof g_plat_enum_impl[0]; i++)
        if (strcmp(g_plat_enum_impl[i].name, name) == 0) return g_plat_enum_impl[i].fn;
    if (plat_is_unity_hook(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_drop(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_options_create(name))
        return kl_named_stub(name, (void *)klplat_options_create);
    if (plat_is_launch_details(name))
        return kl_named_stub(name, (void *)klplat_launch_details);
    if (plat_is_launch_field(name))
        return kl_named_stub(name, (void *)klplat_launch_field);
    if (plat_is_empty_array(name))
        return kl_named_stub(name, (void *)klplat_empty_array);
    if (plat_is_options_sink(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_launch_accessor(name))
        return kl_named_stub(name, (void *)klplat_nothing);
    if (plat_is_init_async(name))
        return kl_named_stub(name, (void *)klplat_init_async);
    if (plat_is_init(name))
        return kl_named_stub(name, (void *)klplat_init_fails);
    if (plat_is_absent(name))
        return kl_named_stub(name, (void *)klplat_absent);
    // Requests before accessors: a request name that happens to contain one of
    // the accessor fragments is still a request.
    if (plat_is_request(name))
        return kl_named_stub(name, (void *)klplat_request_fails);
    if (plat_is_model_accessor(name))
        return kl_named_stub(name, (void *)klplat_model_accessor);
    return kl_named_stub(name, (void *)klplat_called);
}

void kl_ovrplat_report(FILE *f) {
    static int done;
    if (done || !g_nplat) return;
    done = 1;
    unsigned called = 0, drm = 0;
    for (unsigned i = 0; i < g_nplat; i++) {
        if (g_plat[i].calls) called++;
        if (g_plat[i].drm)   drm++;
    }
    fprintf(f, "\n=== Oculus Platform surface ===\n");
    fprintf(f, "  resolved: %u, of which called: %u\n", g_nplat, called);
    if (g_plat_unclassified)
        fprintf(f, "  %u name%s answered 0 from the UNCLASSIFIED tier — each one is "
                   "a line above saying so, and the next round's work list\n",
                g_plat_unclassified, g_plat_unclassified == 1 ? "" : "s");
    if (drm) {
        fprintf(f, "  --- entitlement / ownership (REFUSED by policy) ---\n");
        for (unsigned i = 0; i < g_nplat; i++)
            if (g_plat[i].drm)
                fprintf(f, "    %-52s %s\n", g_plat[i].name,
                        g_plat[i].calls ? "CALLED" : "resolved only");
        fprintf(f, "  (%u DRM call%s refused)\n", g_drm_calls, g_drm_calls == 1 ? "" : "s");
    }
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_nplat; i++)
        if (g_plat[i].calls && !g_plat[i].drm)
            fprintf(f, "    %-52s x%u\n", g_plat[i].name, g_plat[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_nplat; i++)
        if (!g_plat[i].calls && !g_plat[i].drm) fprintf(f, "    %s\n", g_plat[i].name);
}
