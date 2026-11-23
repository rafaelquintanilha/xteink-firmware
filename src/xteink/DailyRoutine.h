#pragma once
// Once-a-day start routine (docs/xteink/sync.md, "Daily routine").
//
// On a power-button or plain boot (never after USB power or a flash), before
// Home or a resumed book opens, the device checks whether it has already run
// today. "Today" starts at 06:00 local time in the configured timezone
// (Asia/Jerusalem when none was ever chosen). When NVS xteink/last_run is
// earlier than the most recent 06:00, the routine runs once:
//
//   battery floors -> Wi-Fi join -> OTA check (and install) -> book sync
//     -> RTC from SNTP
//
// A firmware branch that cannot finish unattended (tunnel-only offer, battery
// below the OTA floor, an install already tried today) never blocks the books:
// the sync still runs in the same start and the firmware is reported deferred.
// The whole routine runs under one hard wall-clock cap and never waits for a
// button.
//
// The original X4 has no RTC and is powered off while asleep on battery, so a
// power-button start there usually has no clock and cannot tell how long the
// device slept. Its starts follow WakePolicy instead of the clock alone:
//   - a start that resumes a book does nothing here: no radio, no wait;
//   - a start that opens Home checks for new content first ("wake check"),
//     whether or not the daily maintenance is due. The join scans first and
//     tries only saved networks in range, the check goes on without a clock
//     when SNTP does not answer, and between daily runs the sync skips the
//     inventory re-hash (XteinkSync.h).
//
// The clock is self-validated on every start: the system clock or the RTC is
// trusted only when it is plausible (not near epoch 0, not behind the
// persisted TrustedTime floor); otherwise Wi-Fi + SNTP establish it first. If
// time still cannot be established the routine does not run and last_run is
// not written (fail closed: the next start tries again).
//
// Everything below the port is pure and host-tested (test/xteink_daily). The
// device bindings are compiled only on the device.
#include <cstddef>
#include <cstdint>
#include <string>

#include "SyncProtocol.h"
#include "XteinkSync.h"

namespace xteink::daily {

// --- constants -------------------------------------------------------------

// The single hard wall-clock cap for one routine, from its first step to the
// end of the sync. An install needs kSyncReserveMs + kOtaMinInstallMs (95 s)
// left after the firmware check, so the cap less 95 s is what the join, the
// clock and the check may take. The X4 starts every routine cold (no RTC: a
// scan-first join plus SNTP) and its first TLS handshake takes seconds; on a
// real X4 those steps took more than 10 s and every install was deferred
// (deferred:time). 125 s leaves them 30 s.
constexpr uint32_t kDailyRoutineCapMs = 125000;
// Below this battery level the routine does not run at all (same floor as the
// sleep-time plugin-event join in main.cpp).
constexpr int kDailyJoinBatteryFloor = 20;
// A firmware install (6 MB flash write, then a reboot and a health check)
// needs more headroom; below this the offer is deferred and the books sync.
constexpr int kDailyOtaBatteryFloor = 40;
// At most this many OTA installs per routine-day. A server that keeps offering
// a build that does not change the reported version (or one that rolls back)
// cannot cause a reboot loop.
constexpr int kMaxOtaInstallsPerDay = 1;
// The routine-day starts at this local hour.
constexpr int kRunHourLocal = 6;
// Asia/Jerusalem (src/util/Timezones.cpp), used when no timezone was chosen.
constexpr const char* kDefaultRoutineTz = "IST-2IDT,M3.4.4/26,M10.5.0";

// Budgets inside the cap.
constexpr uint32_t kJoinBudgetMs = 20000;      // unattended Wi-Fi join
constexpr uint32_t kClockSyncBudgetMs = 8000;  // SNTP when the clock is not trusted
constexpr uint32_t kSyncReserveMs = 35000;     // kept for the book sync after OTA work
// An install with less time left is deferred. A ~6 MB image over the LAN
// costs roughly 30 s when nothing goes wrong (erasing the image's sectors,
// streaming it, writing it); the transfer's per-phase timeout
// (kTransferTimeoutMs in OtaUpdater.cpp) is another 30 s, so a single stall
// that resumes can still finish. With a typical join and check, the cap less
// kSyncReserveMs leaves about 65 s.
constexpr uint32_t kOtaMinInstallMs = 60000;
constexpr uint32_t kSyncTailMs = 10000;  // no new download starts in the last 10 s
// Wake check: SNTP gets this long when the start has no clock. Enough for an
// answer on a working network; the content check does not depend on it.
constexpr uint32_t kWakeClockBudgetMs = 3000;

// Below this the clock was obviously never set (2025-01-01 UTC); the same
// floor TrustedTime uses.
constexpr int64_t kMinPlausibleEpoch = 1735689600LL;
// A last_run this far past "now" is treated as corrupt (the routine is due).
constexpr int64_t kLastRunFutureSlackSecs = 2 * 86400;

// --- pure helpers ------------------------------------------------------------

// How the device came up (HalGPIO::WakeupReason, plus the reboot kinds that
// setup() resolves separately).
enum class BootKind : uint8_t { PowerButton, AfterUSBPower, AfterFlash, Other };

// True when this boot may run the routine: power-button or plain boots only,
// never after USB power or a flash, a silent (heap-defrag) reboot, a panic
// reboot, or in recovery-firmware mode.
bool bootQualifies(BootKind kind, bool silentReboot, bool panicReboot, bool recoveryMode);

enum class Trigger : uint8_t { PowerOn, Wake };
// "power_on" | "wake" (the server's run_trigger values).
const char* triggerName(Trigger t);

// The POSIX TZ rule that defines the 06:00 boundary: the configured clock
// rule, or kDefaultRoutineTz when the user never chose a timezone (the
// shipped clock default is UTC) or the rule is empty.
const char* routineTimezone(bool timezoneChosen, const char* clockTz);

// The most recent local kRunHourLocal:00 at or before nowUtc under posixTz,
// as a UTC epoch; -1 when it cannot be computed (no clock, or a rule that does
// not parse). The rule is evaluated by hand: the process-wide TZ variable is
// never touched, because the render task formats the clock under it.
int64_t mostRecentBoundary(int64_t nowUtc, const char* posixTz, int hour = kRunHourLocal);

// The UTC offset (seconds east of UTC) a POSIX TZ rule ("IST-2IDT,M3.4.4/26,
// M10.5.0", "UTC+05:30", "<+03>-3") puts in force at `utc`. Supports the
// Mm.w.d, Jn and n date forms and rule times from -167 to 167 hours. False when
// the rule does not parse.
bool utcOffsetAt(const char* posixTz, int64_t utc, int32_t& offsetSecs);

// The routine is due when lastRun is earlier than the most recent boundary
// (or is corrupt: far in the future). lastRun <= 0 = never ran.
bool isDue(int64_t nowUtc, int64_t lastRun, int64_t boundary);

// "YYYY-MM-DDTHH:MM:SSZ".
std::string isoUtc(int64_t epoch);

// --- clock self-validation -----------------------------------------------------

enum class ClockSource : uint8_t {
  None,     // nothing trustworthy
  System,   // the system clock kept running (deep sleep, soft reset)
  Rtc,      // the board's RTC
  Network,  // SNTP after a Wi-Fi join
};

const char* clockSourceName(ClockSource s);

// Plausible = not near epoch 0 and not behind the persisted floor.
bool clockPlausible(int64_t epoch, int64_t floor);

struct ClockChoice {
  ClockSource source = ClockSource::None;
  int64_t epoch = 0;
};

// Picks the trusted local clock without the network: the system clock as it
// was at boot (before TrustedTime restored the floor into it), else the RTC.
// rtcEpoch <= 0 = no RTC. `systemNowEpoch` is the system clock now (used when
// the boot reading was plausible, so time spent since boot is counted).
ClockChoice chooseLocalClock(int64_t bootSystemEpoch, int64_t systemNowEpoch, int64_t rtcEpoch, int64_t floor);

// --- outcome strings -------------------------------------------------------------

// content_outcome for a sync: "ok", "cancelled", "partial:<code>" (some books
// arrived), "error:<code>".
std::string contentOutcome(const sync::Outcome& out);

// fw_outcome strings (server pattern ^(none|installed|failed:\S.*|deferred:\S.*)$).
constexpr const char* kFwNone = "none";
constexpr const char* kFwInstalled = "installed";
constexpr const char* kFwDeferredTunnel = "deferred:tunnel";
constexpr const char* kFwDeferredBattery = "deferred:battery";
constexpr const char* kFwDeferredDailyCap = "deferred:daily_cap";
constexpr const char* kFwDeferredTime = "deferred:time";
// The offer names a build this device already rolled back from (rb_list).
constexpr const char* kFwDeferredRolledBack = "deferred:rolled_back";
std::string fwFailed(sync::Error e);  // "failed:<code>"

// Persistent note for the Home status line.
enum class Note : uint8_t {
  None = 0,
  FirmwarePending = 1,  // an offer is waiting (deferred)
  LowBattery = 2,       // today's routine skipped for battery
};

Note noteFromByte(uint8_t b);

// "<syncLine> · <note>", either part alone, or false (buf = "") when both are
// empty. Truncates to len.
bool composeStatusLine(const char* syncLine, const char* noteText, char* buf, size_t len);

// --- orchestration -----------------------------------------------------------------

struct OtaCheck {
  sync::OtaDecision decision = sync::OtaDecision::None;
  sync::Error error = sync::Error::None;  // != None: the check itself failed
};

enum class Stage : uint8_t { Clock, CheckingFirmware, InstallingFirmware, Syncing, WakeCheck };

// Everything the routine needs from the device.
class RoutinePort {
 public:
  virtual ~RoutinePort() = default;
  virtual uint32_t millis() = 0;
  virtual bool provisioned() = 0;  // a device key exists
  virtual int batteryPercent() = 0;

  // Clock sources.
  virtual int64_t bootClockEpoch() = 0;  // system clock before the floor restore
  virtual int64_t systemClockEpoch() = 0;
  virtual int64_t rtcEpoch() = 0;   // <= 0 when there is no RTC reading
  virtual int64_t timeFloor() = 0;  // persisted TrustedTime floor, 0 = none
  virtual void adoptClock(int64_t epoch) = 0;
  // Blocking SNTP over the joined station; the synced epoch or 0.
  virtual int64_t syncClock(uint32_t budgetMs) = 0;
  virtual std::string timezone() = 0;  // POSIX rule for the boundary
  // The background SNTP started by the join has answered (the system clock is
  // network time). Must not rely on sntp_get_sync_status() alone: reading it
  // clears COMPLETED, so an earlier reader may have consumed it.
  virtual bool sntpCompleted() = 0;
  // Waits up to budgetMs for that answer; false at once when SNTP never started.
  virtual bool waitSntp(uint32_t budgetMs) = 0;
  // Writes a UTC epoch to the board's RTC (HalClock::setUtc: no TZ change).
  // False when there is no RTC or the write failed.
  virtual bool writeRtc(int64_t epoch) = 0;
  // The board has an RTC to write. Without one, no run waits for a late SNTP
  // answer.
  virtual bool hasRtc() { return true; }

  // Network.
  virtual bool wifiConnected() = 0;
  virtual bool joinWifi(uint32_t budgetMs) = 0;
  // The wake check's join: scan first, then only saved networks in range
  // (wifijoin::joinAnySaved with scanFirst).
  virtual bool joinWifiInRange(uint32_t budgetMs) { return joinWifi(budgetMs); }
  virtual void wifiOff() = 0;

  // NVS (namespace "xteink").
  virtual int64_t lastRun() = 0;  // "last_run", 0 = never
  virtual void setLastRun(int64_t epoch) = 0;
  virtual int64_t otaDay() = 0;  // "ota_day": boundary of the day of the last install attempt
  virtual void setOtaDay(int64_t boundary) = 0;
  virtual uint8_t otaCount() = 0;  // "ota_n": install attempts on that day
  virtual void setOtaCount(uint8_t n) = 0;
  // "fw_verified": a post-update health check passed and no daily run has
  // reported it yet (the sync clears it once the report is delivered).
  virtual bool verifiedInstall() = 0;
  virtual Note note() = 0;              // "dr_note"
  virtual void setNote(Note note) = 0;  // "dr_note"

  // OTA (OtaUpdater) and the book sync (XteinkSync::run).
  // One status POST for the offer, bounded by budgetMs (both origins).
  virtual OtaCheck checkOta(uint32_t budgetMs) = 0;
  // Installs the checked offer within budgetMs; Error::None = the boot
  // partition now points at the new image.
  virtual sync::Error installOta(uint32_t budgetMs) = 0;
  virtual void reboot() = 0;  // does not return on the device
  virtual sync::Outcome runSync(const sync::RunFields& fields, uint32_t itemBudgetMs, uint32_t hardBudgetMs) = 0;
  // The same sync without the inventory re-hash (XteinkSync::run, quickInventory):
  // a wake check between two daily runs.
  virtual sync::Outcome runQuickSync(const sync::RunFields& fields, uint32_t itemBudgetMs, uint32_t hardBudgetMs) {
    return runSync(fields, itemBudgetMs, hardBudgetMs);
  }

  // Non-blocking progress popup; never waits for input.
  virtual void showStage(Stage stage) { (void)stage; }
};

enum class Status : uint8_t {
  NotDue,                 // already ran this routine-day: nothing done
  Ran,                    // the sync step ran; last_run written
  Rebooting,              // a firmware image was installed; last_run not written
  SkippedNotProvisioned,  // no device key: nothing to sync with
  SkippedNoClock,         // time could not be established (fail closed)
  SkippedLowBattery,      // below kDailyJoinBatteryFloor
  SkippedNoWifi,          // no saved network joined
};

const char* statusName(Status s);

struct Result {
  Status status = Status::NotDue;
  ClockSource clock = ClockSource::None;
  int64_t now = 0;       // trusted epoch the decision used (0 = none)
  int64_t boundary = 0;  // most recent 06:00 (0 = not computed)
  sync::RunFields fields;
  sync::Outcome sync;
  // The RTC got the SNTP time before the radio went off (serial log only; not
  // part of the status report).
  bool rtcWritten = false;
};

// Runs the whole routine against `port`. Never waits on button input. The
// only caller is setup(), before activity routing, so no book can be open
// while it runs (there is no reader check: it could never be true there).
// `checkInbox` selects the wake check described at the top of this file.
Result run(RoutinePort& port, Trigger trigger, bool checkInbox = false);

// What a start does here (decided in setup(), see the top of this file).
enum class WakePolicy : uint8_t {
  Daily,       // the once-a-day routine, when it is due
  CheckInbox,  // the wake check: the daily routine when due, and the content check either way
  Skip,        // nothing: the start resumes a book
};

#ifdef ARDUINO
// Device entry from setup(), after the OTA health check and before activity
// routing. `bootClockEpoch` is time(nullptr) captured before
// trustedtime::init(); `deepSleepWake` selects run_trigger "wake".
void onBoot(GfxRenderer& renderer, BootKind kind, bool silentReboot, bool panicReboot, bool recoveryMode,
            bool deepSleepWake, int64_t bootClockEpoch, WakePolicy policy);

// Note for the Home status line (cached; safe from the render task).
Note homeNote();

// A sync that reached the server clears a stale "low battery" note (a manual
// sync may succeed after today's routine was skipped).
void onSyncSucceeded();

// The server answered a status report with no offer for another build: clear
// an "update pending" note. Otherwise it stays until the next daily check,
// even after the build was installed some other way (USB) or the offer was
// withdrawn.
void onNoFirmwareOffer();
#endif

}  // namespace xteink::daily
