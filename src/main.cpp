// Standalone StopWatch snapshot; original shared firmware remains unchanged.
// Build with the stopwatch_pet PlatformIO environment in this repository.
#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#if !defined(CODEX_PET_STOPWATCH)
#include <M5StackChan.h>
#endif
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <time.h>

#if !defined(CODEX_PET_STOPWATCH)
#include "codex_pet_frames.generated.h"
#else
static constexpr char CODEX_PET_ID[] = "stopwatch";
#endif

// StackChan Codex Pet
//
// The device owns the pet renderer, animation timing, touch interaction, and
// quota LEDs. A small Mac bridge supplies task and quota data over BLE, with
// USB serial retained as a diagnostic and flashing fallback.

static M5Canvas canvas(&M5.Display);
static Preferences preferences;

static constexpr uint32_t HOST_TIMEOUT_MS = 35000;
static constexpr uint32_t BLE_SCAN_INTERVAL_MS = 30000;
static constexpr uint32_t BLE_RECONNECT_WINDOW_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t BLE_READ_INTERVAL_MS = 30000;
static constexpr uint32_t AUTO_SLEEP_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t BADGE_VISIBLE_MS = 3500;
static constexpr uint32_t LED_UPDATE_MS = 500;
static constexpr uint32_t IDLE_MOTION_MIN_MS = 20000;
static constexpr uint32_t IDLE_MOTION_MAX_MS = 45000;
static constexpr uint8_t ACTIVE_BRIGHTNESS = 150;
#if defined(CODEX_PET_STOPWATCH)
static constexpr uint32_t QUOTA_RING_REVEAL_MS = 1100;
static constexpr uint32_t QUOTA_RING_FRAME_MS = 16;
static constexpr uint32_t WORKING_RING_FRAME_MS = 16;
static constexpr uint32_t WORKING_RING_LAP_MS = 3600;
static constexpr int WORKING_RING_DIRTY_CHUNKS = 9;
static constexpr size_t WORKING_RING_TRANSFER_PIXELS = 80UL * 80UL;
#endif

static BLEUUID codexBleServiceUUID("e7d6d101-5a2d-4b7a-9c0e-123456789abc");
static BLEUUID codexBleSnapshotUUID("e7d6d102-5a2d-4b7a-9c0e-123456789abc");
static BLEUUID codexBleActionUUID("e7d6d109-5a2d-4b7a-9c0e-123456789abc");

enum class PetActivity : uint8_t {
    Idle = 0,
    Running,
    NeedsInput,
    Ready,
    Blocked,
};

enum class PetClip : uint8_t {
    Idle = 0,
    RunningRight,
    RunningLeft,
    Waving,
    Jumping,
    Failed,
    Waiting,
    Running,
    Review,
};

enum class MotionPattern : uint8_t {
    None = 0,
    IdleLook,
    Focus,
    Attention,
    Nod,
    Shake,
    Greeting,
};

#if !defined(CODEX_PET_STOPWATCH)
struct ClipData {
    const CodexPetFrame* frames;
    uint8_t count;
};

#endif

static PetActivity activity = PetActivity::Idle;
static PetClip currentClip = PetClip::Idle;
static MotionPattern motionPattern = MotionPattern::None;
static String serialLine;
static uint8_t frameIndex = 0;
static uint8_t completedLoops = 0;
static uint8_t loopLimit = 0;
static uint8_t motionStep = 0;
static uint8_t activeTaskCount = 0;
static int quotaWeekRemaining = -1;
static int quotaFiveHourRemaining = -1;
static int cpuUsagePercent = -1;
static int memoryUsagePercent = -1;
static String modelName = "MODEL --";
#if defined(CODEX_PET_STOPWATCH)
static int weatherTemperatureC = 999;
static int weatherCode = -1;
static bool weatherIsDay = true;
static uint32_t weatherUpdatedEpoch = 0;
#endif
static BLEAdvertisedDevice* bleTargetDevice = nullptr;
static BLEClient* bleClient = nullptr;
static BLERemoteCharacteristic* bleSnapshotCharacteristic = nullptr;
static BLERemoteCharacteristic* bleActionCharacteristic = nullptr;
static SemaphoreHandle_t blePayloadMutex = nullptr;
static String pendingBlePayload;

struct RgbColor
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};
static bool transientClip = false;
static bool displayDirty = true;
static bool awake = true;
static bool reducedMotion = false;
static bool hostConnected = false;
static bool canvasReady = false;
static bool bleInitialized = false;
static bool bleConnected = false;
static bool bleShouldConnect = false;
static bool blePayloadReady = false;
static bool bleDormant = false;
static bool bleDisconnectTracking = false;
static bool classicBtMemoryReleased = false;
static uint32_t frameStartedMs = 0;
static uint32_t badgeUntilMs = 0;
static uint32_t greetingUntilMs = 0;
static uint32_t lastHostMessageMs = 0;
static uint32_t lastActivityMs = 0;
static uint32_t lastLedUpdateMs = 0;
static uint32_t motionStepStartedMs = 0;
static uint32_t nextIdleMotionMs = 0;
static uint32_t lastBleScanMs = 0;
static uint32_t lastBleReadMs = 0;
static uint32_t bleDisconnectedSinceMs = 0;
static constexpr uint8_t BLE_SNAPSHOT_CHUNK_MAGIC = 0xC7;
static constexpr size_t BLE_SNAPSHOT_CHUNK_HEADER_BYTES = 4;
static bool bleSnapshotChunkActive = false;
static uint8_t bleSnapshotChunkSequence = 0;
static uint8_t bleSnapshotChunkNextIndex = 0;
static uint8_t bleSnapshotChunkCount = 0;
static String bleSnapshotChunkPayload;
#if defined(CODEX_PET_STOPWATCH)
static uint8_t lastBurnInShiftPhase = 0xFF;
static bool quotaRingRevealActive = false;
static bool quotaRingRevealPending = false;
static int quotaRingRevealTarget = -1;
static uint32_t quotaRingRevealStartedMs = 0;
static uint32_t quotaRingLastFrameMs = 0;
static uint32_t workingRingLastFrameMs = 0;
static uint32_t workingRingFpsStartedMs = 0;
static uint32_t workingRingRenderedFrames = 0;
static bool workingRingFpsReported = false;
static bool workingRingFramePending = false;
static uint16_t* workingRingBackground = nullptr;
static uint16_t* workingRingTransfer = nullptr;
struct WorkingRingRect
{
    int x;
    int y;
    int width;
    int height;
    bool valid;
};
static WorkingRingRect workingRingPreviousRects[WORKING_RING_DIRTY_CHUNKS] = {};
static float quotaRingLastDrawnPercent = 0.0f;
static float cpuRingLastDrawnPercent = 0.0f;
static float memoryRingLastDrawnPercent = 0.0f;
static bool quotaRingFramePending = false;
static bool watchClockSynced = false;
static int64_t watchClockEpochUtc = 0;
static int32_t watchClockTzOffsetSeconds = 0;
static uint32_t watchClockSyncedMs = 0;
static int64_t lastWatchMinuteKey = -1;
#endif

static bool deadlineReached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

static bool deadlinePending(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(deadline - now) > 0;
}

#if !defined(CODEX_PET_STOPWATCH)
static ClipData clipData(PetClip clip)
{
    switch (clip) {
    case PetClip::RunningRight:
        return { CODEX_PET_RUNNING_RIGHT_FRAMES, CODEX_PET_RUNNING_RIGHT_FRAMES_COUNT };
    case PetClip::RunningLeft:
        return { CODEX_PET_RUNNING_LEFT_FRAMES, CODEX_PET_RUNNING_LEFT_FRAMES_COUNT };
    case PetClip::Waving:
        return { CODEX_PET_WAVING_FRAMES, CODEX_PET_WAVING_FRAMES_COUNT };
    case PetClip::Jumping:
        return { CODEX_PET_JUMPING_FRAMES, CODEX_PET_JUMPING_FRAMES_COUNT };
    case PetClip::Failed:
        return { CODEX_PET_FAILED_FRAMES, CODEX_PET_FAILED_FRAMES_COUNT };
    case PetClip::Waiting:
        return { CODEX_PET_WAITING_FRAMES, CODEX_PET_WAITING_FRAMES_COUNT };
    case PetClip::Running:
        return { CODEX_PET_RUNNING_FRAMES, CODEX_PET_RUNNING_FRAMES_COUNT };
    case PetClip::Review:
        return { CODEX_PET_REVIEW_FRAMES, CODEX_PET_REVIEW_FRAMES_COUNT };
    case PetClip::Idle:
    default:
        return { CODEX_PET_IDLE_FRAMES, CODEX_PET_IDLE_FRAMES_COUNT };
    }
}

#endif

static PetClip clipForActivity(PetActivity value)
{
    switch (value) {
    case PetActivity::Running:
        return PetClip::Running;
    case PetActivity::NeedsInput:
        return PetClip::Waiting;
    case PetActivity::Ready:
        return PetClip::Review;
    case PetActivity::Blocked:
        return PetClip::Failed;
    case PetActivity::Idle:
    default:
        return PetClip::Idle;
    }
}

static const char* activityName(PetActivity value)
{
    switch (value) {
    case PetActivity::Running:
        return "Running";
    case PetActivity::NeedsInput:
        return "Needs input";
    case PetActivity::Ready:
        return "Ready";
    case PetActivity::Blocked:
        return "Blocked";
    case PetActivity::Idle:
    default:
        return "Idle";
    }
}

static const char* clipName(PetClip clip)
{
    switch (clip) {
    case PetClip::RunningRight:
        return "running-right";
    case PetClip::RunningLeft:
        return "running-left";
    case PetClip::Waving:
        return "waving";
    case PetClip::Jumping:
        return "jumping";
    case PetClip::Failed:
        return "failed";
    case PetClip::Waiting:
        return "waiting";
    case PetClip::Running:
        return "running";
    case PetClip::Review:
        return "review";
    case PetClip::Idle:
    default:
        return "idle";
    }
}

static uint16_t activityColor(PetActivity value)
{
    switch (value) {
    case PetActivity::Running:
        return 0x2D7F;
    case PetActivity::NeedsInput:
        return 0xFD20;
    case PetActivity::Ready:
        return 0x4EEB;
    case PetActivity::Blocked:
        return 0xF9E7;
    case PetActivity::Idle:
    default:
        return 0x3DDF;
    }
}

#if !defined(CODEX_PET_STOPWATCH)
static uint32_t frameDurationMs(PetClip clip, uint8_t index)
{
    static const uint16_t idleDurations[] = { 280, 110, 110, 140, 140, 320 };
    if (clip == PetClip::Idle) {
        return static_cast<uint32_t>(idleDurations[index % 6]) * 6UL;
    }

    uint16_t normal = 140;
    uint16_t final = 280;
    switch (clip) {
    case PetClip::RunningRight:
    case PetClip::RunningLeft:
    case PetClip::Running:
        normal = 120;
        final = 220;
        break;
    case PetClip::Failed:
        normal = 140;
        final = 240;
        break;
    case PetClip::Waiting:
        normal = 150;
        final = 260;
        break;
    case PetClip::Review:
        normal = 150;
        final = 280;
        break;
    case PetClip::Waving:
    case PetClip::Jumping:
    case PetClip::Idle:
        break;
    }
    const ClipData data = clipData(clip);
    return index + 1 >= data.count ? final : normal;
}

#endif

static void startClip(PetClip clip, uint8_t loops, bool isTransient)
{
    currentClip = clip;
    frameIndex = 0;
    completedLoops = 0;
    loopLimit = loops;
    transientClip = isTransient;
    frameStartedMs = millis();
    displayDirty = true;
}

static void startActivityClip()
{
    const PetClip clip = clipForActivity(activity);
    startClip(clip, clip == PetClip::Idle ? 0 : 3, false);
}

static void playTransient(PetClip clip, uint8_t loops)
{
    if (!awake) {
        return;
    }
    startClip(clip, loops, true);
}

static void scheduleNextIdleMotion()
{
    nextIdleMotionMs = millis() + random(IDLE_MOTION_MIN_MS, IDLE_MOTION_MAX_MS + 1);
}

static void beginBle();

#if defined(CODEX_PET_STOPWATCH)
static void startQuotaRingReveal()
{
    quotaRingRevealActive = false;
    quotaRingRevealPending = awake && !reducedMotion;
    if (!quotaRingRevealPending || quotaWeekRemaining < 0) {
        displayDirty = true;
        if (quotaRingRevealPending) {
            Serial.println("EVENT quota_ring_reveal waiting_for_quota");
        }
        return;
    }

    quotaRingRevealPending = false;
    quotaRingRevealActive = true;
    quotaRingRevealTarget = constrain(quotaWeekRemaining, 0, 100);
    quotaRingRevealStartedMs = millis();
    quotaRingLastFrameMs = 0;
    quotaRingLastDrawnPercent = 0.0f;
    cpuRingLastDrawnPercent = 0.0f;
    memoryRingLastDrawnPercent = 0.0f;
    quotaRingFramePending = false;
    displayDirty = true;
    Serial.printf("EVENT quota_ring_reveal start target=%d\n", quotaRingRevealTarget);
}
#endif

static void triggerMotion(MotionPattern pattern)
{
    // Physical motion is intentionally disabled. Keep this hook so the Pet
    // state mapping stays readable without ever issuing a servo command.
    (void)pattern;
}

static void centerServosOnce()
{
#if defined(CODEX_PET_STOPWATCH)
    Serial.println("SERVO unavailable on StopWatch");
#else
    Serial.println("SERVO home start");
    M5StackChan.setServoPowerEnabled(true);
    delay(250);
    M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
    M5StackChan.Motion.setTorqueEnabled(true);
    M5StackChan.Motion.goHome(300);

    const uint32_t startedMs = millis();
    while (M5StackChan.Motion.isMoving() && millis() - startedMs < 4000UL) {
        delay(20);
    }
    delay(250);
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.setServoPowerEnabled(false);
    Serial.printf("SERVO home done timeout=%u\n", millis() - startedMs >= 4000UL ? 1 : 0);
#endif
}

static void setAwake(bool value)
{
    if (awake == value) {
        return;
    }
    awake = value;
    preferences.putBool("awake", awake);
    displayDirty = true;
    if (awake) {
        lastActivityMs = millis();
        if (bleDormant) {
            bleDormant = false;
            bleDisconnectTracking = true;
            bleDisconnectedSinceMs = millis();
        }
        beginBle();
        // A physical wake resumes the reconnect window immediately.
        lastBleScanMs = millis() - BLE_SCAN_INTERVAL_MS;
        M5.Display.setBrightness(ACTIVE_BRIGHTNESS);
#if defined(CODEX_PET_STOPWATCH)
        startQuotaRingReveal();
#endif
        greetingUntilMs = millis() + 8000UL;
        playTransient(PetClip::Waving, 3);
        triggerMotion(MotionPattern::Greeting);
    } else {
        // Display sleep is deliberately independent from BLE. A live link is
        // retained, and a disconnected link gets a five-minute retry window.
        motionPattern = MotionPattern::None;
#if defined(CODEX_PET_STOPWATCH)
        quotaRingRevealActive = false;
        quotaRingRevealPending = false;
        quotaRingFramePending = false;
        workingRingFramePending = false;
        for (auto& rect : workingRingPreviousRects) {
            rect.valid = false;
        }
#endif
#if !defined(CODEX_PET_STOPWATCH)
        M5StackChan.setServoPowerEnabled(false);
        M5StackChan.showRgbColor(0, 0, 0);
#endif
        if (canvasReady) {
            canvas.fillScreen(TFT_BLACK);
            canvas.pushSprite(0, 0);
        }
        M5.Display.setBrightness(0);
    }
}

static void noteActivity()
{
    lastActivityMs = millis();
    if (!awake) {
        setAwake(true);
    }
}

static void setActivity(PetActivity next)
{
    if (activity == next) {
        return;
    }
    activity = next;
    badgeUntilMs = millis() + BADGE_VISIBLE_MS;
    displayDirty = true;
    if (!transientClip) {
        startActivityClip();
    }

    switch (activity) {
    case PetActivity::Running:
#if defined(CODEX_PET_STOPWATCH)
        workingRingFpsStartedMs = 0;
        workingRingRenderedFrames = 0;
        workingRingFpsReported = false;
        for (auto& rect : workingRingPreviousRects) {
            rect.valid = false;
        }
#endif
        triggerMotion(MotionPattern::Focus);
        break;
    case PetActivity::NeedsInput:
        triggerMotion(MotionPattern::Attention);
        break;
    case PetActivity::Ready:
        triggerMotion(MotionPattern::Nod);
        break;
    case PetActivity::Blocked:
        triggerMotion(MotionPattern::Shake);
        break;
    case PetActivity::Idle:
        scheduleNextIdleMotion();
        break;
    }
    Serial.printf("STATE activity=%s\n", activityName(activity));
}

static bool parseActivity(String value, PetActivity& parsed)
{
    value.trim();
    value.toLowerCase();
    value.replace('-', '_');
    value.replace(' ', '_');
    if (value == "idle") {
        parsed = PetActivity::Idle;
    } else if (value == "running") {
        parsed = PetActivity::Running;
    } else if (value == "needs_input" || value == "waiting" || value == "warning") {
        parsed = PetActivity::NeedsInput;
    } else if (value == "ready" || value == "review" || value == "done" || value == "success") {
        parsed = PetActivity::Ready;
    } else if (value == "blocked" || value == "failed" || value == "error" || value == "danger") {
        parsed = PetActivity::Blocked;
    } else {
        return false;
    }
    return true;
}

static void printStatus()
{
    Serial.printf(
        "STATUS activity=%s clip=%s frame=%u awake=%u connected=%u ble=%u dormant=%u retry=%u tasks=%u week=%d five_hour=%d cpu=%d memory=%d model=%s pet=%s\n",
        activityName(activity),
        clipName(currentClip),
        static_cast<unsigned>(frameIndex),
        awake ? 1 : 0,
        hostConnected ? 1 : 0,
        bleConnected ? 1 : 0,
        bleDormant ? 1 : 0,
        bleDisconnectTracking ? 1 : 0,
        static_cast<unsigned>(activeTaskCount),
        quotaWeekRemaining,
        quotaFiveHourRemaining,
        cpuUsagePercent,
        memoryUsagePercent,
        modelName.c_str(),
        CODEX_PET_ID);
#if defined(CODEX_PET_STOPWATCH)
    Serial.printf(
        "WEATHER temp=%d code=%d day=%u updated=%lu\n",
        weatherTemperatureC,
        weatherCode,
        weatherIsDay ? 1 : 0,
        static_cast<unsigned long>(weatherUpdatedEpoch));
#endif
}

#if defined(CODEX_PET_STOPWATCH)
static void syncWatchClock(int64_t epochUtc, int32_t timezoneOffsetSeconds, bool persistToRtc)
{
    watchClockEpochUtc = epochUtc;
    watchClockTzOffsetSeconds = constrain(timezoneOffsetSeconds, -18 * 3600, 18 * 3600);
    watchClockSyncedMs = millis();
    watchClockSynced = true;
    lastWatchMinuteKey = -1;
    displayDirty = true;

    if (persistToRtc && M5.Rtc.isEnabled()) {
        const time_t localEpoch = static_cast<time_t>(epochUtc + watchClockTzOffsetSeconds);
        struct tm localTime = {};
        gmtime_r(&localEpoch, &localTime);
        M5.Rtc.setDateTime(&localTime);
    }
}

static void restoreWatchClockFromRtc()
{
    if (!M5.Rtc.isEnabled()) {
        Serial.println("CLOCK rtc unavailable");
        return;
    }
    auto dateTime = M5.Rtc.getDateTime();
    if (dateTime.date.year < 2024 || dateTime.date.year > 2099 ||
        dateTime.date.month < 1 || dateTime.date.month > 12 ||
        dateTime.date.date < 1 || dateTime.date.date > 31) {
        Serial.printf(
            "CLOCK rtc invalid %04d-%02d-%02d\n",
            dateTime.date.year,
            dateTime.date.month,
            dateTime.date.date);
        return;
    }

    struct tm localTime = dateTime.get_tm();
    const time_t localEpoch = mktime(&localTime);
    syncWatchClock(static_cast<int64_t>(localEpoch), 0, false);
    Serial.printf(
        "CLOCK rtc restored %04d-%02d-%02d %02d:%02d\n",
        dateTime.date.year,
        dateTime.date.month,
        dateTime.date.date,
        dateTime.time.hours,
        dateTime.time.minutes);
}
#endif

static void handleCommand(String line)
{
    line.trim();
    if (line.length() == 0) {
        return;
    }
    lastHostMessageMs = millis();
    if (!hostConnected) {
        hostConnected = true;
        displayDirty = true;
    }

    String lower = line;
    lower.toLowerCase();
    if (lower == "ping" || lower == "hello") {
        Serial.printf("PONG codex-pet/1 pet=%s\n", CODEX_PET_ID);
        return;
    }
    if (lower == "status") {
        printStatus();
        return;
    }
    if (lower == "wake") {
        noteActivity();
        setAwake(true);
        Serial.println("OK wake");
        return;
    }
    if (lower == "tuck" || lower == "sleep") {
        setAwake(false);
        Serial.println("OK tuck");
        return;
    }
    if (lower == "servo_home") {
#if defined(CODEX_PET_STOPWATCH)
        Serial.println("ERR servo unavailable");
#else
        centerServosOnce();
        Serial.println("OK servo_home");
#endif
        return;
    }
    if (lower.startsWith("tasks ")) {
        const uint8_t nextTaskCount = constrain(line.substring(6).toInt(), 0, 99);
        if (nextTaskCount != activeTaskCount) {
            noteActivity();
            activeTaskCount = nextTaskCount;
            preferences.putUChar("tasks", activeTaskCount);
            displayDirty = true;
        }
        Serial.printf("OK tasks %u\n", static_cast<unsigned>(activeTaskCount));
        return;
    }
    if (lower.startsWith("quota ")) {
        int week = -1;
        int fiveHour = -1;
        if (sscanf(line.c_str(), "%*s %d %d", &week, &fiveHour) != 2) {
            Serial.println("ERR quota <week_remaining> <five_hour_remaining>");
            return;
        }
        const int nextWeek = week < 0 ? -1 : constrain(week, 0, 100);
        const int nextFiveHour = fiveHour < 0 ? -1 : constrain(fiveHour, 0, 100);
        if (nextWeek != quotaWeekRemaining) {
            quotaWeekRemaining = nextWeek;
            preferences.putInt("week", quotaWeekRemaining);
        }
        if (nextFiveHour != quotaFiveHourRemaining) {
            quotaFiveHourRemaining = nextFiveHour;
            preferences.putInt("five_hour", quotaFiveHourRemaining);
        }
        lastLedUpdateMs = 0;
#if defined(CODEX_PET_STOPWATCH)
        if (quotaRingRevealPending && quotaWeekRemaining >= 0) {
            startQuotaRingReveal();
        } else if (quotaRingRevealActive && quotaWeekRemaining >= 0) {
            quotaRingRevealTarget = quotaWeekRemaining;
        }
        displayDirty = true;
#endif
        Serial.printf("OK quota week=%d five_hour=%d\n", quotaWeekRemaining, quotaFiveHourRemaining);
        return;
    }
    if (lower.startsWith("model ")) {
        String nextModel = line.substring(6);
        nextModel.trim();
        if (nextModel.length() == 0) {
            nextModel = "MODEL --";
        }
        if (nextModel.length() > 24) {
            nextModel.remove(24);
        }
        if (nextModel != modelName) {
            modelName = nextModel;
            preferences.putString("model", modelName);
            displayDirty = true;
        }
        Serial.printf("OK model %s\n", modelName.c_str());
        return;
    }
#if defined(CODEX_PET_STOPWATCH)
    if (lower.startsWith("system ")) {
        int cpu = -1;
        int memory = -1;
        if (sscanf(line.c_str(), "%*s %d %d", &cpu, &memory) != 2) {
            Serial.println("ERR system <cpu_percent> <memory_percent>");
            return;
        }
        const int nextCpu = cpu < 0 ? -1 : constrain(cpu, 0, 100);
        const int nextMemory = memory < 0 ? -1 : constrain(memory, 0, 100);
        if (nextCpu != cpuUsagePercent || nextMemory != memoryUsagePercent) {
            cpuUsagePercent = nextCpu;
            memoryUsagePercent = nextMemory;
            displayDirty = true;
        }
        Serial.printf("OK system cpu=%d memory=%d\n", cpuUsagePercent, memoryUsagePercent);
        return;
    }
    if (lower.startsWith("weather ")) {
        int temperature = 999;
        int code = -1;
        int isDay = 1;
        unsigned long updated = 0;
        if (sscanf(line.c_str(), "%*s %d %d %d %lu", &temperature, &code, &isDay, &updated) != 4 ||
            temperature < -80 || temperature > 60 || code < 0 || code > 99) {
            Serial.println("ERR weather <temp_c> <wmo_code> <is_day> <updated_epoch>");
            return;
        }
        const bool changed = temperature != weatherTemperatureC || code != weatherCode ||
                             (isDay != 0) != weatherIsDay || updated != weatherUpdatedEpoch;
        weatherTemperatureC = temperature;
        weatherCode = code;
        weatherIsDay = isDay != 0;
        weatherUpdatedEpoch = static_cast<uint32_t>(updated);
        if (changed) {
            preferences.putInt("wx_temp", weatherTemperatureC);
            preferences.putInt("wx_code", weatherCode);
            preferences.putBool("wx_day", weatherIsDay);
            preferences.putUInt("wx_epoch", weatherUpdatedEpoch);
            displayDirty = true;
        }
        Serial.printf(
            "OK weather temp=%d code=%d day=%u updated=%lu\n",
            weatherTemperatureC,
            weatherCode,
            weatherIsDay ? 1 : 0,
            static_cast<unsigned long>(weatherUpdatedEpoch));
        return;
    }
    if (lower.startsWith("clock ")) {
        long long epochUtc = 0;
        int timezoneOffsetSeconds = 0;
        if (sscanf(line.c_str(), "%*s %lld %d", &epochUtc, &timezoneOffsetSeconds) != 2 ||
            epochUtc < 1600000000LL) {
            Serial.println("ERR clock <epoch_utc> <timezone_offset_seconds>");
            return;
        }
        syncWatchClock(epochUtc, timezoneOffsetSeconds, true);
        Serial.printf(
            "OK clock epoch=%lld tz=%d rtc=%u\n",
            epochUtc,
            timezoneOffsetSeconds,
            M5.Rtc.isEnabled() ? 1 : 0);
        return;
    }
#endif
    if (lower.startsWith("reduced_motion ")) {
        reducedMotion = line.substring(15).toInt() != 0;
        preferences.putBool("reduced", reducedMotion);
        transientClip = false;
        startActivityClip();
        Serial.printf("OK reduced_motion %u\n", reducedMotion ? 1 : 0);
        return;
    }
    if (lower.startsWith("state ")) {
        PetActivity parsed;
        if (parseActivity(line.substring(6), parsed)) {
            // Repeated BLE snapshots are heartbeats, not user activity. Wake
            // the display only when the visible Codex state really changes.
            if (parsed != activity) {
                noteActivity();
                preferences.putUChar("activity", static_cast<uint8_t>(parsed));
            }
            setActivity(parsed);
            Serial.printf("OK state %s\n", activityName(parsed));
        } else {
            Serial.println("ERR state idle|running|needs_input|ready|blocked");
        }
        return;
    }
    if (lower == "wave") {
        playTransient(PetClip::Waving, 3);
        triggerMotion(MotionPattern::Greeting);
        Serial.println("OK wave");
        return;
    }
    if (lower == "jump") {
        playTransient(PetClip::Jumping, 1);
        triggerMotion(MotionPattern::Nod);
        Serial.println("OK jump");
        return;
    }
    if (lower == "run_left") {
        playTransient(PetClip::RunningLeft, 1);
        Serial.println("OK run_left");
        return;
    }
    if (lower == "run_right") {
        playTransient(PetClip::RunningRight, 1);
        Serial.println("OK run_right");
        return;
    }
    Serial.println("ERR unknown command");
}

static void updateSerial()
{
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            handleCommand(serialLine);
            serialLine = "";
        } else if (serialLine.length() < 180) {
            serialLine += c;
        } else {
            serialLine = "";
            Serial.println("ERR line too long");
        }
    }
}

static String bleValueForKey(const String& payload, const char* key)
{
    const String prefix = String(key) + "=";
    int start = 0;
    while (start < static_cast<int>(payload.length())) {
        int end = payload.indexOf('\n', start);
        if (end < 0) {
            end = payload.length();
        }
        String line = payload.substring(start, end);
        line.trim();
        if (line.startsWith(prefix)) {
            return line.substring(prefix.length());
        }
        start = end + 1;
    }
    return "";
}

static void applyBleSnapshot(const String& payload)
{
    if (bleValueForKey(payload, "PROTOCOL") != "codex-pet/1") {
        Serial.println("BLE snapshot ignored protocol");
        return;
    }
    const String state = bleValueForKey(payload, "STATE");
    const String tasks = bleValueForKey(payload, "TASKS");
    const String week = bleValueForKey(payload, "WEEK");
    const String fiveHour = bleValueForKey(payload, "FIVE_HOUR");
    const String model = bleValueForKey(payload, "MODEL");
#if defined(CODEX_PET_STOPWATCH)
    const String epoch = bleValueForKey(payload, "E");
    const String timezoneOffset = bleValueForKey(payload, "TZ");
    const String weatherTemp = bleValueForKey(payload, "WT");
    const String weatherWmo = bleValueForKey(payload, "WC");
    const String weatherDay = bleValueForKey(payload, "WD");
    const String weatherUpdated = bleValueForKey(payload, "WU");
    const String cpu = bleValueForKey(payload, "CPU");
    const String memory = bleValueForKey(payload, "MEM");
    if (epoch.length() > 0) {
        const int64_t parsedEpoch = strtoll(epoch.c_str(), nullptr, 10);
        if (parsedEpoch >= 1600000000LL) {
            syncWatchClock(parsedEpoch, timezoneOffset.toInt(), true);
        }
    }
    if (weatherTemp.length() > 0 && weatherWmo.length() > 0 && weatherDay.length() > 0 &&
        weatherUpdated.length() > 0) {
        handleCommand(
            "weather " + weatherTemp + " " + weatherWmo + " " + weatherDay + " " + weatherUpdated);
    }
    if (cpu.length() > 0 && memory.length() > 0) {
        handleCommand("system " + cpu + " " + memory);
    }
#endif
    if (state.length() > 0) {
        handleCommand("state " + state);
    }
    if (tasks.length() > 0) {
        handleCommand("tasks " + tasks);
    }
    if (week.length() > 0 && fiveHour.length() > 0) {
        handleCommand("quota " + week + " " + fiveHour);
    }
    if (model.length() > 0) {
        handleCommand("model " + model);
    }
    Serial.printf("BLE snapshot applied bytes=%u\n", static_cast<unsigned>(payload.length()));
}

static void queueBleSnapshot(const uint8_t* data, size_t length)
{
    if (blePayloadMutex == nullptr || xSemaphoreTake(blePayloadMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    if (length >= BLE_SNAPSHOT_CHUNK_HEADER_BYTES && data[0] == BLE_SNAPSHOT_CHUNK_MAGIC) {
        const uint8_t sequence = data[1];
        const uint8_t index = data[2];
        const uint8_t count = data[3];
        if (count == 0 || index >= count) {
            bleSnapshotChunkActive = false;
            bleSnapshotChunkPayload = "";
            xSemaphoreGive(blePayloadMutex);
            return;
        }
        if (index == 0) {
            bleSnapshotChunkActive = true;
            bleSnapshotChunkSequence = sequence;
            bleSnapshotChunkNextIndex = 0;
            bleSnapshotChunkCount = count;
            bleSnapshotChunkPayload = "";
            bleSnapshotChunkPayload.reserve(static_cast<unsigned int>(count) *
                                            (length - BLE_SNAPSHOT_CHUNK_HEADER_BYTES));
        }
        if (!bleSnapshotChunkActive || sequence != bleSnapshotChunkSequence ||
            count != bleSnapshotChunkCount || index != bleSnapshotChunkNextIndex) {
            bleSnapshotChunkActive = false;
            bleSnapshotChunkPayload = "";
            xSemaphoreGive(blePayloadMutex);
            return;
        }
        for (size_t i = BLE_SNAPSHOT_CHUNK_HEADER_BYTES; i < length; ++i) {
            bleSnapshotChunkPayload += static_cast<char>(data[i]);
        }
        ++bleSnapshotChunkNextIndex;
        if (bleSnapshotChunkNextIndex == bleSnapshotChunkCount) {
            pendingBlePayload = bleSnapshotChunkPayload;
            blePayloadReady = true;
            bleSnapshotChunkActive = false;
            bleSnapshotChunkPayload = "";
        }
        xSemaphoreGive(blePayloadMutex);
        return;
    }

    bleSnapshotChunkActive = false;
    bleSnapshotChunkPayload = "";
    pendingBlePayload = "";
    pendingBlePayload.reserve(length + 1);
    for (size_t i = 0; i < length; ++i) {
        pendingBlePayload += static_cast<char>(data[i]);
    }
    blePayloadReady = true;
    xSemaphoreGive(blePayloadMutex);
}

static void bleNotifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t length, bool)
{
    queueBleSnapshot(data, length);
}

static void clearBleLink()
{
    bleConnected = false;
    bleSnapshotCharacteristic = nullptr;
    bleActionCharacteristic = nullptr;
    if (!bleDormant && !bleDisconnectTracking) {
        bleDisconnectTracking = true;
        bleDisconnectedSinceMs = millis();
        lastBleScanMs = millis() - BLE_SCAN_INTERVAL_MS;
        Serial.println("BLE reconnect window started");
    }
}

class CodexBleClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient*) override
    {
        bleDisconnectTracking = false;
        Serial.println("BLE link connected");
    }

    void onDisconnect(BLEClient*) override
    {
        clearBleLink();
        Serial.println("BLE link disconnected");
    }
};

class CodexBleAdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override
    {
        const bool matchingService = advertisedDevice.haveServiceUUID() &&
                                     advertisedDevice.isAdvertisingService(codexBleServiceUUID);
        const bool matchingName = advertisedDevice.haveName() &&
                                  advertisedDevice.getName() == "StackChanCodex";
        if (!matchingService && !matchingName) {
            return;
        }
        Serial.printf("BLE host found name=%s rssi=%d\n",
                      advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "",
                      advertisedDevice.getRSSI());
        BLEDevice::getScan()->stop();
        if (bleTargetDevice != nullptr) {
            delete bleTargetDevice;
        }
        bleTargetDevice = new BLEAdvertisedDevice(advertisedDevice);
        bleShouldConnect = true;
    }
};

static void beginBle()
{
    if (bleInitialized) {
        return;
    }
    if (!classicBtMemoryReleased) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        classicBtMemoryReleased = true;
    }
    BLEDevice::init(
#if defined(CODEX_PET_STOPWATCH)
        "StopWatchCodexPet"
#else
        "StackChanCodexPet"
#endif
    );
    if (blePayloadMutex == nullptr) {
        blePayloadMutex = xSemaphoreCreateMutex();
    }
    bleInitialized = true;
    bleDormant = false;
    bleDisconnectTracking = true;
    bleDisconnectedSinceMs = millis();
    lastBleScanMs = millis() - BLE_SCAN_INTERVAL_MS;
    Serial.println("BLE central ready");
}

static void suspendBle()
{
    if (!bleInitialized) {
        return;
    }
    bleDormant = true;
    bleDisconnectTracking = false;
    if (bleClient != nullptr && bleClient->isConnected()) {
        bleClient->disconnect();
    }
    clearBleLink();
    hostConnected = false;
    displayDirty = true;
    bleShouldConnect = false;
    if (bleTargetDevice != nullptr) {
        delete bleTargetDevice;
        bleTargetDevice = nullptr;
    }
    if (blePayloadMutex != nullptr && xSemaphoreTake(blePayloadMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        pendingBlePayload = "";
        blePayloadReady = false;
        bleSnapshotChunkActive = false;
        bleSnapshotChunkPayload = "";
        xSemaphoreGive(blePayloadMutex);
    }
    // Keep NimBLE allocated so a physical wake can resume it safely.
    Serial.println("BLE dormant (scan/reconnect stopped)");
}

static void scanForBleHost()
{
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new CodexBleAdvertisedCallbacks(), false);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->setActiveScan(true);
    Serial.println("BLE scan start");
#if defined(CODEX_PET_STOPWATCH)
    BLEScanResults results = scan->start(3, false);
    Serial.printf("BLE scan complete count=%d\n", results.getCount());
#else
    BLEScanResults* results = scan->start(3, false);
    if (results != nullptr) {
        Serial.printf("BLE scan complete count=%d\n", results->getCount());
    }
#endif
    scan->clearResults();
    lastBleScanMs = millis();
}

static bool connectBleHost()
{
    if (bleTargetDevice == nullptr) {
        return false;
    }
    if (bleClient == nullptr) {
        bleClient = BLEDevice::createClient();
        bleClient->setClientCallbacks(new CodexBleClientCallbacks());
        bleClient->setMTU(512);
    }
    if (!bleClient->connect(bleTargetDevice)) {
        Serial.println("BLE host connect failed");
        clearBleLink();
        return false;
    }
    BLERemoteService* service = bleClient->getService(codexBleServiceUUID);
    if (service == nullptr) {
        Serial.println("BLE service missing");
        bleClient->disconnect();
        clearBleLink();
        return false;
    }
    bleSnapshotCharacteristic = service->getCharacteristic(codexBleSnapshotUUID);
    bleActionCharacteristic = service->getCharacteristic(codexBleActionUUID);
    if (bleSnapshotCharacteristic == nullptr || !bleSnapshotCharacteristic->canRead()) {
        Serial.println("BLE snapshot characteristic missing");
        bleClient->disconnect();
        clearBleLink();
        return false;
    }
    if (bleSnapshotCharacteristic->canNotify()) {
        bleSnapshotCharacteristic->registerForNotify(bleNotifyCallback);
    }
#if defined(CODEX_PET_STOPWATCH)
    const std::string initialValue = bleSnapshotCharacteristic->readValue();
    const String initial(initialValue.c_str());
#else
    const String initial = bleSnapshotCharacteristic->readValue();
#endif
    if (initial.length() > 0) {
        applyBleSnapshot(initial);
    }
    bleConnected = true;
    bleDisconnectTracking = false;
    lastBleReadMs = millis();
    Serial.println("BLE host ready");
    if (!awake) {
        setAwake(true);
        Serial.println("EVENT ble_reconnect_wake");
    }
    return true;
}

static bool sendBleAction(const char* action)
{
    if (!bleConnected || bleActionCharacteristic == nullptr ||
        (!bleActionCharacteristic->canWrite() && !bleActionCharacteristic->canWriteNoResponse())) {
        return false;
    }
    const size_t length = strlen(action);
    bleActionCharacteristic->writeValue(
        reinterpret_cast<uint8_t*>(const_cast<char*>(action)),
        length,
        bleActionCharacteristic->canWrite());
    Serial.printf("BLE action sent %s\n", action);
    return true;
}

static void updateBle()
{
    if (!bleInitialized || bleDormant) {
        return;
    }
    if (blePayloadReady && blePayloadMutex != nullptr &&
        xSemaphoreTake(blePayloadMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        const String payload = pendingBlePayload;
        pendingBlePayload = "";
        blePayloadReady = false;
        xSemaphoreGive(blePayloadMutex);
        applyBleSnapshot(payload);
    }
    if (bleShouldConnect) {
        bleShouldConnect = false;
        connectBleHost();
        if (bleTargetDevice != nullptr) {
            delete bleTargetDevice;
            bleTargetDevice = nullptr;
        }
    }
    if (bleConnected && (bleClient == nullptr || !bleClient->isConnected())) {
        clearBleLink();
    }
    const uint32_t now = millis();
    if (!bleConnected && !bleDisconnectTracking) {
        bleDisconnectTracking = true;
        bleDisconnectedSinceMs = now;
        lastBleScanMs = now - BLE_SCAN_INTERVAL_MS;
        Serial.println("BLE reconnect window started");
    }
    if (!bleConnected && bleDisconnectTracking &&
        now - bleDisconnectedSinceMs >= BLE_RECONNECT_WINDOW_MS) {
        setAwake(false);
        suspendBle();
        Serial.println("EVENT ble_retry_timeout");
        return;
    }
    if (!bleConnected && !bleShouldConnect && now - lastBleScanMs >= BLE_SCAN_INTERVAL_MS) {
        scanForBleHost();
    } else if (bleConnected && bleSnapshotCharacteristic != nullptr &&
               now - lastBleReadMs >= BLE_READ_INTERVAL_MS) {
        lastBleReadMs = now;
#if defined(CODEX_PET_STOPWATCH)
        const std::string snapshotValue = bleSnapshotCharacteristic->readValue();
        const String snapshot(snapshotValue.c_str());
#else
        const String snapshot = bleSnapshotCharacteristic->readValue();
#endif
        if (snapshot.length() > 0) {
            applyBleSnapshot(snapshot);
        }
    }
}

static void updateAnimation()
{
#if defined(CODEX_PET_STOPWATCH)
    // The StopWatch face is time-only. Pet clips can still represent state in
    // diagnostics, but must not trigger redundant full-screen refreshes.
    return;
#else
    if (!awake) {
        return;
    }
    const uint32_t now = millis();
    if (reducedMotion) {
        // Reduced motion shows one representative frame. Transient gestures
        // still yield to the current activity after a short static pose.
        if (transientClip && now - frameStartedMs >= 900UL) {
            transientClip = false;
            startActivityClip();
        }
        return;
    }
    if (now - frameStartedMs < frameDurationMs(currentClip, frameIndex)) {
        return;
    }
    frameStartedMs = now;
    const ClipData data = clipData(currentClip);
    ++frameIndex;
    if (frameIndex >= data.count) {
        frameIndex = 0;
        ++completedLoops;
        if (loopLimit > 0 && completedLoops >= loopLimit) {
            if (transientClip) {
                transientClip = false;
                startActivityClip();
            } else {
                startClip(PetClip::Idle, 0, false);
            }
        }
    }
    displayDirty = true;
#endif
}

static void updateMotion()
{
    // Servo power stays off for the entire runtime.
}

static RgbColor quotaColorForPercent(int percent)
{
    if (percent < 0) {
        return { 0, 0, 0 };
    }
    if (percent <= 20) {
        return { 72, 0, 0 };
    }
    if (percent <= 50) {
        return { 72, 36, 0 };
    }
    return { 0, 64, 72 };
}

static void setWeeklyQuotaLeds(int percent, const RgbColor& color)
{
#if defined(CODEX_PET_STOPWATCH)
    (void)percent;
    (void)color;
#else
    const RgbColor depleted = { 30, 0, 5 };
    const int progressUnits = percent < 0 ? 0 : constrain(percent, 0, 100) * 12;
    for (uint8_t i = 0; i < 12; ++i) {
        RgbColor output = percent < 0 ? RgbColor { 0, 0, 0 } : depleted;
        const int fill = constrain(progressUnits - static_cast<int>(i) * 100, 0, 100);
        if (fill > 0) {
            const int level = max(fill, 18);
            output.r = static_cast<uint8_t>((static_cast<uint16_t>(color.r) * level) / 100);
            output.g = static_cast<uint8_t>((static_cast<uint16_t>(color.g) * level) / 100);
            output.b = static_cast<uint8_t>((static_cast<uint16_t>(color.b) * level) / 100);
        }
        // Preserve the previous per-side growth direction while treating both
        // sides as one 12-step gauge: left base, right base, then upward.
        const uint8_t sideOffset = i / 2;
        const uint8_t physicalIndex = (i & 1) == 0 ? 5 - sideOffset : 6 + sideOffset;
        M5StackChan.setRgbColor(physicalIndex, output.r, output.g, output.b);
    }
#endif
}

static void updateLeds()
{
    const uint32_t now = millis();
    if (now - lastLedUpdateMs < LED_UPDATE_MS) {
        return;
    }
    lastLedUpdateMs = now;
    if (!awake) {
        return;
    }

    const RgbColor color = quotaColorForPercent(quotaWeekRemaining);
    setWeeklyQuotaLeds(quotaWeekRemaining, color);
#if !defined(CODEX_PET_STOPWATCH)
    M5StackChan.refreshRgb();
#endif
}

#if defined(CODEX_PET_STOPWATCH)
static uint8_t burnInShiftPhase()
{
    return static_cast<uint8_t>((millis() / 60000UL) % 8UL);
}

static int burnInShiftX()
{
    static const int8_t shifts[] = { -1, 0, 1, 1, 1, 0, -1, -1 };
    return shifts[burnInShiftPhase()];
}

static int burnInShiftY()
{
    static const int8_t shifts[] = { 1, 1, 1, 0, -1, -1, -1, 0 };
    return shifts[burnInShiftPhase()];
}

static float ringRevealProgress()
{
    if (!quotaRingRevealActive) {
        return 1.0f;
    }
    const uint32_t elapsed = millis() - quotaRingRevealStartedMs;
    if (elapsed >= QUOTA_RING_REVEAL_MS) {
        return 1.0f;
    }
    const float linear = elapsed / static_cast<float>(QUOTA_RING_REVEAL_MS);
    return linear * linear * (3.0f - 2.0f * linear);
}

static float displayedMetricRingPercent(int percent)
{
    if (percent < 0) {
        return -1.0f;
    }
    return constrain(percent * ringRevealProgress(), 0.0f, 100.0f);
}

static int availablePercent(int usedPercent)
{
    return usedPercent < 0 ? -1 : 100 - constrain(usedPercent, 0, 100);
}

static void drawDashedRingPortion(
    int cx,
    int cy,
    int outerRadius,
    int innerRadius,
    float startAngle,
    float endAngle,
    uint16_t color)
{
    static constexpr float DASH_DEGREES = 5.5f;
    static constexpr float DASH_STEP_DEGREES = 9.0f;
    const int dashOuterRadius = outerRadius - 1;
    const int dashInnerRadius = innerRadius + 3;
    for (float angle = startAngle; angle < endAngle; angle += DASH_STEP_DEGREES) {
        const float dashEnd = angle + DASH_DEGREES < endAngle ? angle + DASH_DEGREES : endAngle;
        canvas.fillArc(
            cx, cy, dashOuterRadius, dashInnerRadius, angle, dashEnd, color);
    }
}

static void drawMetricRingSegment(
    int cx,
    int cy,
    int outerRadius,
    int innerRadius,
    float startAngle,
    int remainingPercent,
    uint16_t consumedColor,
    uint16_t valueColor)
{
    static constexpr float SEGMENT_SWEEP = 112.0f;
    const float segmentEnd = startAngle + SEGMENT_SWEEP;
    if (remainingPercent < 0) {
        drawDashedRingPortion(
            cx, cy, outerRadius, innerRadius, startAngle, segmentEnd, consumedColor);
        return;
    }

    const float remainingEnd = startAngle +
        SEGMENT_SWEEP * constrain(remainingPercent, 0, 100) / 100.0f;
    drawDashedRingPortion(
        cx, cy, outerRadius, innerRadius, remainingEnd, segmentEnd, consumedColor);

    const float displayed = displayedMetricRingPercent(remainingPercent);
    if (displayed > 0.0f) {
        canvas.fillArc(
            cx,
            cy,
            outerRadius,
            innerRadius,
            startAngle,
            startAngle + SEGMENT_SWEEP * displayed / 100.0f,
            valueColor);
    }
}

static uint8_t workingRingChannel(uint8_t low, uint8_t high, float pulse)
{
    return static_cast<uint8_t>(
        low + static_cast<float>(high - low) * constrain(pulse, 0.0f, 1.0f));
}

static void drawWorkingOrbitTrail(int cx, int cy, float headAngle)
{
    // Direction 01: a wave travels through fixed bezel ticks. Nothing rotates
    // physically, so the eye reads the motion without a glowing solid arc.
    static constexpr int ACTIVE_TICKS = WORKING_RING_DIRTY_CHUNKS;
    static constexpr float HALF_WAVE_TICKS = 4.5f;
    static constexpr int OUTER_RADIUS = 228;
    const float headTick = (headAngle + 90.0f) / 6.0f;
    const int firstTick = static_cast<int>(floorf(headTick)) - 4;
    for (int piece = 0; piece < ACTIVE_TICKS; ++piece) {
        const int tick = firstTick + piece;
        const float distance = fabsf(static_cast<float>(tick) - headTick);
        if (distance >= HALF_WAVE_TICKS) {
            continue;
        }
        const float envelope = sinf(
            (1.0f - distance / HALF_WAVE_TICKS) * PI * 0.5f);
        const float shaped = envelope * envelope;
        const int normalizedTick = ((tick % 60) + 60) % 60;
        const bool major = (normalizedTick % 5) == 0;
        const int length = (major ? 12 : 8) +
            static_cast<int>(lroundf(7.0f * shaped));
        const int thickness = 1 + static_cast<int>(lroundf(2.0f * shaped));
        const float angle = (-90.0f + tick * 6.0f) * DEG_TO_RAD;
        const float radialX = cosf(angle);
        const float radialY = sinf(angle);
        const float tangentX = -radialY;
        const float tangentY = radialX;
        const float strength = 0.18f + shaped * 0.82f;
        const uint16_t color = canvas.color565(
            workingRingChannel(23, 76, strength),
            workingRingChannel(56, 184, strength),
            workingRingChannel(60, 195, strength));
        for (int width = -(thickness / 2); width <= thickness / 2; ++width) {
            const float offsetX = tangentX * width;
            const float offsetY = tangentY * width;
            canvas.drawLine(
                cx + static_cast<int>(lroundf(radialX * (OUTER_RADIUS - length) + offsetX)),
                cy + static_cast<int>(lroundf(radialY * (OUTER_RADIUS - length) + offsetY)),
                cx + static_cast<int>(lroundf(radialX * OUTER_RADIUS + offsetX)),
                cy + static_cast<int>(lroundf(radialY * OUTER_RADIUS + offsetY)),
                color);
        }
    }
}

static WorkingRingRect workingOrbitBounds(int cx, int cy, float angleDegrees)
{
    static constexpr float OUTER_RADIUS = 232.0f;
    static constexpr float INNER_RADIUS = 204.0f;
    const float angle = angleDegrees * DEG_TO_RAD;
    const float x1 = cx + cosf(angle) * INNER_RADIUS;
    const float y1 = cy + sinf(angle) * INNER_RADIUS;
    const float x2 = cx + cosf(angle) * OUTER_RADIUS;
    const float y2 = cy + sinf(angle) * OUTER_RADIUS;
    const int left = max(0, static_cast<int>(floorf(min(x1, x2))) - 4);
    const int top = max(0, static_cast<int>(floorf(min(y1, y2))) - 4);
    const int right = min(canvas.width(), static_cast<int>(ceilf(max(x1, x2))) + 5);
    const int bottom = min(canvas.height(), static_cast<int>(ceilf(max(y1, y2))) + 5);
    return { left, top, max(0, right - left), max(0, bottom - top), true };
}

static WorkingRingRect unionWorkingRingRects(
    const WorkingRingRect& first,
    const WorkingRingRect& second)
{
    if (!first.valid) {
        return second;
    }
    if (!second.valid) {
        return first;
    }
    const int left = min(first.x, second.x);
    const int top = min(first.y, second.y);
    const int right = max(first.x + first.width, second.x + second.width);
    const int bottom = max(first.y + first.height, second.y + second.height);
    return { left, top, right - left, bottom - top, true };
}

static void restoreWorkingRingRect(const WorkingRingRect& rect)
{
    if (!rect.valid || workingRingBackground == nullptr || canvas.getBuffer() == nullptr) {
        return;
    }
    auto* destination = static_cast<uint16_t*>(canvas.getBuffer());
    const int stride = canvas.width();
    for (int row = 0; row < rect.height; ++row) {
        const size_t offset = static_cast<size_t>(rect.y + row) * stride + rect.x;
        memcpy(destination + offset, workingRingBackground + offset,
               static_cast<size_t>(rect.width) * sizeof(uint16_t));
    }
}

static void drawQuotaRing()
{
    const int cx = canvas.width() / 2 + burnInShiftX();
    const int cy = canvas.height() / 2 + burnInShiftY();

    // The bezel is intentionally neutral. Resource values live in the three
    // labelled instruments below, leaving this edge free for the working
    // orbit and making its motion uninterrupted and semantically unambiguous.
    for (int tick = 0; tick < 60; ++tick) {
        const float angle = (-90.0f + tick * 6.0f) * DEG_TO_RAD;
        const bool major = (tick % 5) == 0;
        const int innerRadius = major ? 214 : 220;
        const int outerRadius = 228;
        const uint16_t color = major
            ? canvas.color565(66, 76, 86)
            : canvas.color565(34, 43, 51);
        canvas.drawLine(
            cx + static_cast<int>(lroundf(cosf(angle) * innerRadius)),
            cy + static_cast<int>(lroundf(sinf(angle) * innerRadius)),
            cx + static_cast<int>(lroundf(cosf(angle) * outerRadius)),
            cy + static_cast<int>(lroundf(sinf(angle) * outerRadius)),
            color);
    }
    if (activity == PetActivity::Running && !reducedMotion &&
        (workingRingBackground == nullptr || workingRingTransfer == nullptr)) {
        const float phase = (millis() % WORKING_RING_LAP_MS) /
            static_cast<float>(WORKING_RING_LAP_MS);
        drawWorkingOrbitTrail(cx, cy, -90.0f + phase * 360.0f);
    }

    if (quotaRingRevealActive) {
        quotaRingLastDrawnPercent = max(0.0f, displayedMetricRingPercent(quotaWeekRemaining));
        cpuRingLastDrawnPercent = max(0.0f, displayedMetricRingPercent(cpuUsagePercent));
        memoryRingLastDrawnPercent = max(0.0f, displayedMetricRingPercent(memoryUsagePercent));
    }
}

static void drawMetricGaugeOnDisplay(
    int centerX,
    int y,
    int percent,
    float& lastDrawnPercent,
    uint16_t color)
{
    const float displayed = displayedMetricRingPercent(percent);
    if (displayed <= lastDrawnPercent) {
        return;
    }
    static constexpr int BLOCKS = 10;
    static constexpr int BLOCK_WIDTH = 7;
    static constexpr int BLOCK_GAP = 3;
    static constexpr int GAUGE_WIDTH = BLOCKS * BLOCK_WIDTH + (BLOCKS - 1) * BLOCK_GAP;
    const int startX = centerX - GAUGE_WIDTH / 2;
    const uint16_t emptyColor = M5.Display.color565(31, 37, 44);
    for (int block = 0; block < BLOCKS; ++block) {
        const int x = startX + block * (BLOCK_WIDTH + BLOCK_GAP);
        M5.Display.fillRect(x, y, BLOCK_WIDTH, 6, emptyColor);
        const float blockProgress = constrain((displayed - block * 10.0f) / 10.0f, 0.0f, 1.0f);
        const int fillWidth = static_cast<int>(lroundf(BLOCK_WIDTH * blockProgress));
        if (fillWidth > 0) {
            M5.Display.fillRect(x, y, fillWidth, 6, color);
        }
    }
    lastDrawnPercent = displayed;
}

static void drawQuotaRingIncrement()
{
    if (!quotaRingFramePending || !quotaRingRevealActive || !awake) {
        return;
    }
    quotaRingFramePending = false;

    const int shiftX = burnInShiftX();
    const int shiftY = burnInShiftY();
    drawMetricGaugeOnDisplay(
        145 + shiftX, 340 + shiftY, quotaWeekRemaining,
        quotaRingLastDrawnPercent, M5.Display.color565(74, 111, 188));
    drawMetricGaugeOnDisplay(
        240 + shiftX, 340 + shiftY, cpuUsagePercent,
        cpuRingLastDrawnPercent, M5.Display.color565(184, 68, 74));
    drawMetricGaugeOnDisplay(
        335 + shiftX, 340 + shiftY, memoryUsagePercent,
        memoryRingLastDrawnPercent, M5.Display.color565(190, 148, 38));
}
#endif

#if defined(CODEX_PET_STOPWATCH)
static void drawWeatherGlyph(int code, bool isDay, int x, int y, uint16_t color)
{
    const bool fog = code == 45 || code == 48;
    const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    const bool thunder = code >= 95;
    const bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || thunder;
    const bool cloud = code >= 1;

    if (!cloud) {
        canvas.drawCircle(x, y, 8, color);
        if (isDay) {
            for (int angle = 0; angle < 360; angle += 45) {
                const float radians = angle * DEG_TO_RAD;
                canvas.drawLine(
                    x + static_cast<int>(cosf(radians) * 11),
                    y + static_cast<int>(sinf(radians) * 11),
                    x + static_cast<int>(cosf(radians) * 15),
                    y + static_cast<int>(sinf(radians) * 15),
                    color);
            }
        } else {
            canvas.fillCircle(x + 5, y - 4, 8, TFT_BLACK);
        }
        return;
    }

    if (fog) {
        canvas.drawFastHLine(x - 14, y - 7, 24, color);
        canvas.drawFastHLine(x - 9, y, 24, color);
        canvas.drawFastHLine(x - 14, y + 7, 24, color);
        return;
    }

    canvas.fillCircle(x - 6, y - 1, 7, color);
    canvas.fillCircle(x + 3, y - 5, 9, color);
    canvas.fillRoundRect(x - 14, y - 1, 30, 10, 5, color);
    if (snow) {
        canvas.fillCircle(x - 7, y + 15, 2, color);
        canvas.fillCircle(x + 7, y + 15, 2, color);
    } else if (rain) {
        canvas.drawLine(x - 9, y + 12, x - 11, y + 17, color);
        canvas.drawLine(x + 1, y + 12, x - 1, y + 17, color);
        canvas.drawLine(x + 11, y + 12, x + 9, y + 17, color);
        if (thunder) {
            canvas.drawLine(x + 2, y + 10, x - 2, y + 17, color);
            canvas.drawLine(x - 2, y + 17, x + 4, y + 15, color);
        }
    }
}
#endif

#if defined(CODEX_PET_STOPWATCH)
static void drawMetricGaugeOnCanvas(int centerX, int y, int percent, uint16_t color)
{
    static constexpr int BLOCKS = 10;
    static constexpr int BLOCK_WIDTH = 7;
    static constexpr int BLOCK_GAP = 3;
    static constexpr int GAUGE_WIDTH = BLOCKS * BLOCK_WIDTH + (BLOCKS - 1) * BLOCK_GAP;
    const int startX = centerX - GAUGE_WIDTH / 2;
    const uint16_t emptyColor = canvas.color565(31, 37, 44);
    const float displayed = max(0.0f, displayedMetricRingPercent(percent));
    for (int block = 0; block < BLOCKS; ++block) {
        const int x = startX + block * (BLOCK_WIDTH + BLOCK_GAP);
        canvas.fillRect(x, y, BLOCK_WIDTH, 6, emptyColor);
        const float blockProgress = constrain((displayed - block * 10.0f) / 10.0f, 0.0f, 1.0f);
        const int fillWidth = static_cast<int>(lroundf(BLOCK_WIDTH * blockProgress));
        if (fillWidth > 0) {
            canvas.fillRect(x, y, fillWidth, 6, color);
        }
    }
}
#endif

#if defined(CODEX_PET_STOPWATCH)
static const char* pixelGlyph3x5(char ch)
{
    switch (ch) {
    case 'A': return "010101111101101";
    case 'B': return "110101110101110";
    case 'C': return "011100100100011";
    case 'D': return "110101101101110";
    case 'E': return "111100110100111";
    case 'F': return "111100110100100";
    case 'G': return "011100101101011";
    case 'H': return "101101111101101";
    case 'I': return "111010010010111";
    case 'J': return "001001001101010";
    case 'K': return "101101110101101";
    case 'L': return "100100100100111";
    case 'M': return "101111111101101";
    case 'N': return "101111111111101";
    case 'O': return "010101101101010";
    case 'P': return "110101110100100";
    case 'Q': return "010101101011001";
    case 'R': return "110101110101101";
    case 'S': return "011100010001110";
    case 'T': return "111010010010010";
    case 'U': return "101101101101111";
    case 'V': return "101101101101010";
    case 'W': return "101101111111101";
    case 'X': return "101101010101101";
    case 'Y': return "101101010010010";
    case 'Z': return "111001010100111";
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "110001010100111";
    case '3': return "110001010001110";
    case '4': return "101101111001001";
    case '5': return "111100110001110";
    case '6': return "011100110101010";
    case '7': return "111001010010010";
    case '8': return "010101010101010";
    case '9': return "010101011001110";
    case '%': return "101001010100101";
    case '-': return "000000111000000";
    case '.': return "000000000000010";
    case '/': return "001001010100100";
    default: return "000000000000000";
    }
}

static int pixelTextWidth(const char* text, int scale, int tracking)
{
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    int width = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        width += (*cursor == ' ') ? 3 * scale : 3 * scale + tracking;
    }
    return max(0, width - tracking);
}

static void drawPixelText(
    const char* text,
    int x,
    int y,
    int scale,
    int tracking,
    uint16_t color)
{
    if (text == nullptr) {
        return;
    }
    int cursorX = x;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        char ch = *cursor;
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
        if (ch == ' ') {
            cursorX += 3 * scale;
            continue;
        }
        const char* pattern = pixelGlyph3x5(ch);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (pattern[row * 3 + column] == '1') {
                    canvas.fillRect(
                        cursorX + column * scale,
                        y + row * scale,
                        scale,
                        scale,
                        color);
                }
            }
        }
        cursorX += 3 * scale + tracking;
    }
}

static void drawPixelTextCentered(
    const char* text,
    int centerX,
    int y,
    int scale,
    int tracking,
    uint16_t color)
{
    drawPixelText(
        text,
        centerX - pixelTextWidth(text, scale, tracking) / 2,
        y,
        scale,
        tracking,
        color);
}

static const char* matrixDigit5x9(char digit)
{
    switch (digit) {
    case '0': return "01110" "11011" "11011" "11011" "11011" "11011" "11011" "11011" "01110";
    case '1': return "00110" "01110" "00110" "00110" "00110" "00110" "00110" "00110" "01111";
    case '2': return "01110" "11011" "00011" "00011" "00110" "01100" "11000" "11000" "11111";
    case '3': return "11110" "00011" "00011" "00110" "00110" "00011" "00011" "11011" "01110";
    case '4': return "00011" "00111" "01111" "11011" "11011" "11111" "00011" "00011" "00011";
    case '5': return "11111" "11000" "11000" "11110" "00011" "00011" "00011" "11011" "01110";
    case '6': return "01110" "11000" "11000" "11110" "11011" "11011" "11011" "11011" "01110";
    case '7': return "11111" "00011" "00011" "00110" "00110" "01100" "01100" "11000" "11000";
    case '8': return "01110" "11011" "11011" "01110" "11011" "11011" "11011" "11011" "01110";
    case '9': return "01110" "11011" "11011" "11011" "01111" "00011" "00011" "00011" "01110";
    default: return "00000" "00000" "00000" "00000" "11111" "00000" "00000" "00000" "00000";
    }
}

static void drawMatrixDigit(
    char digit,
    int x,
    int y,
    uint16_t litColor,
    uint16_t ghostColor)
{
    static constexpr int COLUMN_PITCH = 9;
    static constexpr int ROW_PITCH = 8;
    static constexpr int CELL_SIZE = 7;
    const char* pattern = matrixDigit5x9(digit);
    for (int row = 0; row < 9; ++row) {
        for (int column = 0; column < 5; ++column) {
            const bool lit = pattern[row * 5 + column] == '1';
            canvas.fillRect(
                x + column * COLUMN_PITCH,
                y + row * ROW_PITCH,
                CELL_SIZE,
                CELL_SIZE,
                lit ? litColor : ghostColor);
        }
    }
}

static void drawMatrixTime(
    const String& hours,
    const String& minutes,
    int shiftX,
    int shiftY,
    uint16_t litColor,
    uint16_t ghostColor,
    uint16_t liveColor)
{
    static constexpr int DIGIT_ADVANCE = 52;
    static constexpr int COLON_ADVANCE = 20;
    int x = 130 + shiftX;
    const int y = 148 + shiftY;
    for (int index = 0; index < 2; ++index) {
        drawMatrixDigit(hours[index], x, y, litColor, ghostColor);
        x += DIGIT_ADVANCE;
    }
    canvas.fillRect(x + 3, y + 24, 7, 7, liveColor);
    canvas.fillRect(x + 3, y + 48, 7, 7, liveColor);
    x += COLON_ADVANCE;
    for (int index = 0; index < 2; ++index) {
        drawMatrixDigit(minutes[index], x, y, litColor, ghostColor);
        x += DIGIT_ADVANCE;
    }
}
#endif

static void drawBadge()
{
    const int displayWidth = canvas.width();
#if defined(CODEX_PET_STOPWATCH)
    const int shiftX = burnInShiftX();
    const int shiftY = burnInShiftY();
    const uint16_t hourTextColor = canvas.color565(218, 224, 223);
    const uint16_t minuteTextColor = canvas.color565(158, 174, 178);
    const uint16_t secondaryTextColor = canvas.color565(94, 108, 114);
    uint16_t statusColor = canvas.color565(76, 96, 106);

    const char* statusText = "IDLE";
    switch (activity) {
    case PetActivity::Running:
        statusText = "WORKING";
        statusColor = canvas.color565(55, 117, 124);
        break;
    case PetActivity::NeedsInput:
        statusText = "NEEDS INPUT";
        statusColor = canvas.color565(164, 111, 48);
        break;
    case PetActivity::Ready:
        statusText = "READY";
        statusColor = canvas.color565(61, 128, 91);
        break;
    case PetActivity::Blocked:
        statusText = "BLOCKED";
        statusColor = canvas.color565(148, 66, 70);
        break;
    case PetActivity::Idle:
    default:
        break;
    }

    struct tm localTime = {};
    String hourText = "--";
    String minuteText = "--";
    String dateText = "TIME SYNC";
    if (watchClockSynced) {
        const uint32_t elapsedSeconds = (millis() - watchClockSyncedMs) / 1000UL;
        const time_t localEpoch = static_cast<time_t>(
            watchClockEpochUtc + watchClockTzOffsetSeconds + elapsedSeconds);
        gmtime_r(&localEpoch, &localTime);
        char hourBuffer[3];
        char minuteBuffer[3];
        snprintf(hourBuffer, sizeof(hourBuffer), "%02d", localTime.tm_hour);
        snprintf(minuteBuffer, sizeof(minuteBuffer), "%02d", localTime.tm_min);
        hourText = hourBuffer;
        minuteText = minuteBuffer;
        static const char* WEEKDAYS[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
        static const char* MONTHS[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
        char dateBuffer[18];
        snprintf(
            dateBuffer,
            sizeof(dateBuffer),
            "%s %02d  %s",
            MONTHS[constrain(localTime.tm_mon, 0, 11)],
            localTime.tm_mday,
            WEEKDAYS[constrain(localTime.tm_wday, 0, 6)]);
        dateText = dateBuffer;
    }

    // Industrial Matrix typography: all Latin information shares a sampled
    // grid and tabular center lines. Semantic color is reserved for live
    // Codex state and the three resource channels.
    const uint16_t timeLitColor = canvas.color565(216, 215, 205);
    const uint16_t matrixGhostColor = canvas.color565(20, 26, 23);
    const uint16_t liveCyan = canvas.color565(51, 184, 190);
    const uint16_t weatherBaseColor = canvas.color565(85, 142, 164);
    const uint16_t dividerColor = canvas.color565(35, 46, 52);
    const bool hasWeather = weatherTemperatureC >= -80 && weatherTemperatureC <= 60 && weatherCode >= 0;
    drawPixelTextCentered(
        dateText.c_str(),
        (hasWeather ? 160 : displayWidth / 2) + shiftX,
        74 + shiftY,
        2,
        2,
        timeLitColor);
    if (hasWeather) {
        uint16_t weatherColor = weatherBaseColor;
        if (watchClockSynced && weatherUpdatedEpoch > 0) {
            const uint32_t currentEpoch = static_cast<uint32_t>(
                watchClockEpochUtc + (millis() - watchClockSyncedMs) / 1000UL);
            if (currentEpoch > weatherUpdatedEpoch + 3UL * 60UL * 60UL) {
                weatherColor = canvas.color565(64, 75, 80);
            }
        }
        drawWeatherGlyph(weatherCode, weatherIsDay, 287 + shiftX, 80 + shiftY, weatherColor);
        const String temperatureText = String(weatherTemperatureC);
        const int temperatureX = 311 + shiftX;
        drawPixelText(temperatureText.c_str(), temperatureX, 74 + shiftY, 2, 2, weatherColor);
        const int temperatureWidth = pixelTextWidth(temperatureText.c_str(), 2, 2);
        canvas.drawRect(
            temperatureX + temperatureWidth + 3,
            72 + shiftY,
            4,
            4,
            weatherColor);
        drawPixelText(
            "C",
            temperatureX + temperatureWidth + 11,
            74 + shiftY,
            2,
            2,
            weatherColor);
        canvas.setFont(&fonts::efontCN_16);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(secondaryTextColor, TFT_BLACK);
        canvas.drawString("余杭", 337 + shiftX, 101 + shiftY);
    }

    drawMatrixTime(
        hourText,
        minuteText,
        shiftX,
        shiftY,
        timeLitColor,
        matrixGhostColor,
        liveCyan);

    canvas.drawFastHLine(120 + shiftX, 238 + shiftY, 240, dividerColor);
    canvas.drawFastHLine(144 + shiftX, 327 + shiftY, 192, dividerColor);
    canvas.drawRect(146 + shiftX, 267 + shiftY, 10, 10, statusColor);
    canvas.fillRect(149 + shiftX, 270 + shiftY, 4, 4, statusColor);
    drawPixelTextCentered(
        statusText,
        240 + shiftX,
        267 + shiftY,
        3,
        3,
        statusColor);

    String compactModel = modelName;
    compactModel.toUpperCase();
    if (compactModel.length() > 15) {
        compactModel = compactModel.substring(0, 15);
    }
    drawPixelTextCentered(
        compactModel.c_str(),
        240 + shiftX,
        297 + shiftY,
        2,
        2,
        hostConnected ? secondaryTextColor : canvas.color565(67, 75, 79));

    const uint16_t quotaColor = canvas.color565(74, 111, 188);
    const uint16_t cpuColor = canvas.color565(184, 68, 74);
    const uint16_t memoryColor = canvas.color565(190, 148, 38);
    drawMetricGaugeOnCanvas(145 + shiftX, 340 + shiftY, quotaWeekRemaining, quotaColor);
    drawMetricGaugeOnCanvas(240 + shiftX, 340 + shiftY, cpuUsagePercent, cpuColor);
    drawMetricGaugeOnCanvas(335 + shiftX, 340 + shiftY, memoryUsagePercent, memoryColor);

    drawPixelTextCentered("QUOTA", 145 + shiftX, 358 + shiftY, 2, 2, quotaColor);
    drawPixelTextCentered("CPU", 240 + shiftX, 358 + shiftY, 2, 2, cpuColor);
    drawPixelTextCentered("MEM", 335 + shiftX, 358 + shiftY, 2, 2, memoryColor);

    const String quotaValue = quotaWeekRemaining < 0 ? "--" : String(quotaWeekRemaining) + "%";
    const String cpuValue = cpuUsagePercent < 0 ? "--" : String(cpuUsagePercent) + "%";
    const String memoryValue = memoryUsagePercent < 0 ? "--" : String(memoryUsagePercent) + "%";
    drawPixelTextCentered(quotaValue.c_str(), 145 + shiftX, 378 + shiftY, 4, 3, quotaColor);
    drawPixelTextCentered(cpuValue.c_str(), 240 + shiftX, 378 + shiftY, 4, 3, cpuColor);
    drawPixelTextCentered(memoryValue.c_str(), 335 + shiftX, 378 + shiftY, 4, 3, memoryColor);
    drawPixelTextCentered("LEFT", 145 + shiftX, 407 + shiftY, 2, 2, secondaryTextColor);
    drawPixelTextCentered("USED", 240 + shiftX, 407 + shiftY, 2, 2, secondaryTextColor);
    drawPixelTextCentered("USED", 335 + shiftX, 407 + shiftY, 2, 2, secondaryTextColor);
#else
    const int modelWidth = min(displayWidth - 76, 22 + static_cast<int>(modelName.length()) * 7);
    const int modelX = (displayWidth - modelWidth) / 2;
    const int badgeCenterY = 24;
    canvas.fillRoundRect(modelX, badgeCenterY - 12, modelWidth, 24, 12, 0x1082);
    canvas.fillCircle(modelX + 12, badgeCenterY, 4, activityColor(activity));
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(TFT_WHITE, 0x1082);
    canvas.setTextSize(1);
    canvas.drawString(modelName, modelX + 22, badgeCenterY);

    const uint16_t connectionColor = hostConnected ? 0x4EEB : 0x7BEF;
    canvas.fillCircle(displayWidth - 34, badgeCenterY, 5, connectionColor);
    if (activeTaskCount > 1) {
        const int taskBadgeX = displayWidth - 58;
        canvas.fillCircle(taskBadgeX, badgeCenterY, 10, activityColor(activity));
        canvas.setFont(&fonts::Font0);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(TFT_BLACK, activityColor(activity));
        canvas.setTextSize(1);
        canvas.drawNumber(activeTaskCount, taskBadgeX, badgeCenterY);
    }

#endif
}

#if defined(CODEX_PET_STOPWATCH)
static void saveWorkingRingBackground()
{
    for (auto& rect : workingRingPreviousRects) {
        rect.valid = false;
    }
    if (workingRingBackground == nullptr || canvas.getBuffer() == nullptr) {
        return;
    }
    const size_t pixels = static_cast<size_t>(canvas.width()) * canvas.height();
    memcpy(workingRingBackground, canvas.getBuffer(), pixels * sizeof(uint16_t));
}

static void drawWorkingOrbitFrame()
{
    if (!workingRingFramePending) {
        return;
    }
    workingRingFramePending = false;
    if (!awake || activity != PetActivity::Running || reducedMotion ||
        workingRingBackground == nullptr || workingRingTransfer == nullptr) {
        return;
    }

    const float phase = (millis() % WORKING_RING_LAP_MS) /
        static_cast<float>(WORKING_RING_LAP_MS);
    const float headAngle = -90.0f + phase * 360.0f;
    const int cx = canvas.width() / 2 + burnInShiftX();
    const int cy = canvas.height() / 2 + burnInShiftY();
    const float headTick = phase * 60.0f;
    const int firstTick = static_cast<int>(floorf(headTick)) - 4;
    WorkingRingRect currentRects[WORKING_RING_DIRTY_CHUNKS];
    for (int chunk = 0; chunk < WORKING_RING_DIRTY_CHUNKS; ++chunk) {
        const float tickAngle = -90.0f + (firstTick + chunk) * 6.0f;
        currentRects[chunk] = workingOrbitBounds(cx, cy, tickAngle);
        restoreWorkingRingRect(workingRingPreviousRects[chunk]);
    }
    drawWorkingOrbitTrail(cx, cy, headAngle);

    const auto* canvasPixels = static_cast<const uint16_t*>(canvas.getBuffer());
    const int stride = canvas.width();
    size_t totalTransferPixels = 0;
    for (int chunk = 0; chunk < WORKING_RING_DIRTY_CHUNKS; ++chunk) {
        const WorkingRingRect transferRect = unionWorkingRingRects(
            workingRingPreviousRects[chunk], currentRects[chunk]);
        const size_t transferPixels =
            static_cast<size_t>(transferRect.width) * transferRect.height;
        if (!transferRect.valid || transferPixels > WORKING_RING_TRANSFER_PIXELS) {
            displayDirty = true;
            for (auto& rect : workingRingPreviousRects) {
                rect.valid = false;
            }
            return;
        }
        M5.Display.waitDisplay();
        for (int row = 0; row < transferRect.height; ++row) {
            const size_t sourceOffset =
                static_cast<size_t>(transferRect.y + row) * stride + transferRect.x;
            memcpy(
                workingRingTransfer + static_cast<size_t>(row) * transferRect.width,
                canvasPixels + sourceOffset,
                static_cast<size_t>(transferRect.width) * sizeof(uint16_t));
        }
        M5.Display.pushImageDMA(
            transferRect.x,
            transferRect.y,
            transferRect.width,
            transferRect.height,
            workingRingTransfer);
        workingRingPreviousRects[chunk] = currentRects[chunk];
        totalTransferPixels += transferPixels;
    }

    const uint32_t now = millis();
    if (workingRingFpsStartedMs == 0) {
        workingRingFpsStartedMs = now;
        workingRingRenderedFrames = 0;
    }
    ++workingRingRenderedFrames;
    const uint32_t measuredMs = now - workingRingFpsStartedMs;
    if (!workingRingFpsReported && measuredMs >= 3000UL) {
        const uint32_t fpsTenths = workingRingRenderedFrames * 10000UL / measuredMs;
        Serial.printf(
            "EVENT working_wavefront fps=%lu.%lu pixels=%lu\n",
            static_cast<unsigned long>(fpsTenths / 10UL),
            static_cast<unsigned long>(fpsTenths % 10UL),
            static_cast<unsigned long>(totalTransferPixels));
        workingRingFpsReported = true;
    }
}
#endif

static void drawPet()
{
    if (!awake || !canvasReady) {
        return;
    }
    canvas.fillScreen(TFT_BLACK);
#if defined(CODEX_PET_STOPWATCH)
    drawQuotaRing();
#endif
#if defined(CODEX_PET_STOPWATCH)
    drawBadge();
#else
    const ClipData data = clipData(currentClip);
    const uint8_t safeIndex = min(frameIndex, static_cast<uint8_t>(data.count - 1));
    const CodexPetFrame& frame = data.frames[safeIndex];
    const int spriteX = (canvas.width() - CODEX_PET_FRAME_WIDTH) / 2;
    const int spriteY = (canvas.height() - CODEX_PET_FRAME_HEIGHT) / 2;
    canvas.drawJpg(frame.data, frame.size, spriteX, spriteY);
    drawBadge();
#endif
#if defined(CODEX_PET_STOPWATCH)
    saveWorkingRingBackground();
#endif
    canvas.pushSprite(0, 0);
#if defined(CODEX_PET_STOPWATCH)
    if (activity == PetActivity::Running && !reducedMotion) {
        workingRingFramePending = true;
    }
#endif
}

static void handleTouch()
{
#if defined(CODEX_PET_STOPWATCH)
    if (!M5.Touch.getCount()) {
        return;
    }
    const auto& touch = M5.Touch.getDetail();
    if (touch.wasFlicked()) {
        noteActivity();
        if (touch.distanceY() > 35) {
            if (awake) {
                setAwake(false);
            }
            Serial.println("EVENT tuck");
        } else if (touch.distanceY() < -35) {
            setAwake(true);
            playTransient(PetClip::Waving, 2);
            Serial.println("EVENT wake");
        } else {
            playTransient(touch.distanceX() < 0 ? PetClip::RunningLeft : PetClip::RunningRight, 1);
        }
        return;
    }
    if (touch.wasClicked()) {
        noteActivity();
        playTransient(PetClip::Jumping, 1);
        M5.Power.setVibration(100);
        delay(20);
        M5.Power.setVibration(0);
        Serial.println("EVENT activate");
        sendBleAction("activate");
    }
#else
    auto& touch = M5StackChan.TouchSensor;
    if (touch.wasSwipedForward()) {
        noteActivity();
        if (!awake) {
            setAwake(true);
        } else {
            playTransient(PetClip::RunningRight, 1);
        }
        Serial.println("EVENT wake");
        return;
    }
    if (touch.wasSwipedBackward()) {
        if (awake) {
            setAwake(false);
        }
        Serial.println("EVENT tuck");
        return;
    }
    if (touch.wasClicked()) {
        noteActivity();
        if (!awake) {
            setAwake(true);
        } else {
            playTransient(PetClip::Jumping, 1);
            triggerMotion(MotionPattern::Nod);
        }
        Serial.println("EVENT activate");
        sendBleAction("activate");
    }
#endif
}

static void handlePhysicalButton()
{
#if defined(CODEX_PET_STOPWATCH)
    if (M5.BtnB.wasClicked()) {
        noteActivity();
        playTransient(PetClip::Waving, 2);
        Serial.println("EVENT wave");
        return;
    }
    if (!M5.BtnA.wasClicked() && !M5.BtnPWR.wasClicked()) {
        return;
    }
#else
    if (!M5.BtnPWR.wasClicked()) {
        return;
    }
#endif
    if (awake) {
        setAwake(false);
        Serial.println("EVENT button_sleep");
    } else {
        setAwake(true);
        Serial.println("EVENT button_wake");
    }
}

static void updateAutoSleep()
{
    if (awake && millis() - lastActivityMs >= AUTO_SLEEP_MS) {
        setAwake(false);
        Serial.println("EVENT auto_sleep");
    }
}

static void updateHostConnection()
{
    if (hostConnected && millis() - lastHostMessageMs > HOST_TIMEOUT_MS) {
        hostConnected = false;
        displayDirty = true;
        if (activity == PetActivity::Running) {
            setActivity(PetActivity::Idle);
        }
    }
}

static void updateDisplayProtection()
{
#if defined(CODEX_PET_STOPWATCH)
    const uint8_t phase = burnInShiftPhase();
    if (phase != lastBurnInShiftPhase) {
        lastBurnInShiftPhase = phase;
        displayDirty = true;
    }
#endif
}

static void updateWatchClock()
{
#if defined(CODEX_PET_STOPWATCH)
    if (!watchClockSynced || !awake) {
        return;
    }
    const uint32_t elapsedSeconds = (millis() - watchClockSyncedMs) / 1000UL;
    const int64_t minuteKey =
        (watchClockEpochUtc + watchClockTzOffsetSeconds + elapsedSeconds) / 60LL;
    if (minuteKey != lastWatchMinuteKey) {
        lastWatchMinuteKey = minuteKey;
        displayDirty = true;
    }
#endif
}

static void updateQuotaRingReveal()
{
#if defined(CODEX_PET_STOPWATCH)
    if (!quotaRingRevealActive || !awake) {
        return;
    }
    const uint32_t now = millis();
    if (now - quotaRingRevealStartedMs >= QUOTA_RING_REVEAL_MS) {
        quotaRingRevealActive = false;
        quotaRingFramePending = false;
        displayDirty = true;
        Serial.printf("EVENT quota_ring_reveal done target=%d\n", quotaRingRevealTarget);
        return;
    }
    if (now - quotaRingLastFrameMs >= QUOTA_RING_FRAME_MS) {
        quotaRingLastFrameMs = now;
        quotaRingFramePending = true;
    }
#endif
}

static void updateWorkingOrbitRing()
{
#if defined(CODEX_PET_STOPWATCH)
    if (!awake || activity != PetActivity::Running || reducedMotion) {
        return;
    }
    const uint32_t now = millis();
    if (now - workingRingLastFrameMs >= WORKING_RING_FRAME_MS) {
        workingRingLastFrameMs = now;
        if (workingRingBackground != nullptr && workingRingTransfer != nullptr) {
            workingRingFramePending = true;
        } else {
            // Allocation failure falls back to the original full-frame path.
            displayDirty = true;
        }
    }
#endif
}

void setup()
{
    Serial.begin(115200);
    delay(60);
#if defined(CODEX_PET_STOPWATCH)
    auto config = M5.config();
    config.serial_baudrate = 115200;
    config.internal_spk = false;
    config.internal_mic = false;
    config.internal_imu = false;
    config.internal_rtc = true;
    M5.begin(config);
#else
    M5StackChan.begin();
    M5StackChan.setServoPowerEnabled(false);
    M5.Display.setRotation(1);
#endif
    M5.Display.setBrightness(ACTIVE_BRIGHTNESS);
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    canvasReady = canvas.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
    if (!canvasReady) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Pet canvas failed", 20, 100);
        Serial.println("FATAL pet canvas allocation failed");
    }
    if (canvasReady) {
        canvas.fillScreen(TFT_BLACK);
        canvas.pushSprite(0, 0);
    }
#if defined(CODEX_PET_STOPWATCH)
    if (canvasReady) {
        const size_t backgroundPixels =
            static_cast<size_t>(M5.Display.width()) * M5.Display.height();
        workingRingBackground = static_cast<uint16_t*>(heap_caps_malloc(
            backgroundPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        workingRingTransfer = static_cast<uint16_t*>(heap_caps_malloc(
            WORKING_RING_TRANSFER_PIXELS * sizeof(uint16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        Serial.printf(
            "WORKING orbit buffers background=%u transfer=%u\n",
            workingRingBackground != nullptr ? 1 : 0,
            workingRingTransfer != nullptr ? 1 : 0);
    }
#endif

    preferences.begin("codex-pet", false);
#if defined(CODEX_PET_STOPWATCH)
    restoreWatchClockFromRtc();
#endif
    awake = preferences.getBool("awake", true);
    reducedMotion = preferences.getBool("reduced", false);
    activeTaskCount = preferences.getUChar("tasks", 0);
    quotaWeekRemaining = preferences.getInt("week", -1);
    quotaFiveHourRemaining = preferences.getInt("five_hour", -1);
    modelName = preferences.getString("model", "MODEL --");
#if defined(CODEX_PET_STOPWATCH)
    weatherTemperatureC = preferences.getInt("wx_temp", 999);
    weatherCode = preferences.getInt("wx_code", -1);
    weatherIsDay = preferences.getBool("wx_day", true);
    weatherUpdatedEpoch = preferences.getUInt("wx_epoch", 0);
#endif
    const uint8_t storedActivity = preferences.getUChar("activity", static_cast<uint8_t>(PetActivity::Idle));
    activity = storedActivity <= static_cast<uint8_t>(PetActivity::Blocked)
                   ? static_cast<PetActivity>(storedActivity)
                   : PetActivity::Idle;
    randomSeed(esp_random());
    lastActivityMs = millis();
    scheduleNextIdleMotion();
    greetingUntilMs = millis() + 8000UL;
    startClip(PetClip::Waving, 3, true);
    triggerMotion(MotionPattern::Greeting);
#if defined(CODEX_PET_STOPWATCH)
    if (awake) {
        startQuotaRingReveal();
    }
#endif
    // Even when the persisted display state is asleep, give BLE five minutes
    // to reconnect. A successful reconnect wakes the display automatically.
    beginBle();
    if (!awake) {
        M5.Display.setBrightness(0);
    }

    Serial.printf("READY codex-pet/1 pet=%s states=5\n", CODEX_PET_ID);
    printStatus();
}

void loop()
{
#if defined(CODEX_PET_STOPWATCH)
    M5.update();
#else
    M5StackChan.update();
#endif
    handlePhysicalButton();
    handleTouch();
    updateSerial();
    updateBle();
    updateHostConnection();
    updateAutoSleep();
    updateDisplayProtection();
    updateWatchClock();
    updateQuotaRingReveal();
    updateWorkingOrbitRing();
    updateAnimation();
    updateMotion();
    updateLeds();

    const uint32_t now = millis();
    static bool badgeWasVisible = true;
    const bool badgeVisible = deadlinePending(now, badgeUntilMs) || deadlinePending(now, greetingUntilMs);
    if (badgeWasVisible != badgeVisible) {
        badgeWasVisible = badgeVisible;
        displayDirty = true;
    }
    if (displayDirty) {
        displayDirty = false;
        drawPet();
    }
#if defined(CODEX_PET_STOPWATCH)
    drawQuotaRingIncrement();
    drawWorkingOrbitFrame();
#endif
    delay(8);
}
