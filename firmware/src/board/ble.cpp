#include "board/ble.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string.h>

// NimBLE's headers do not compile clean under the project's warning set — they
// shadow declarations in inline members and widen float literals — and they
// arrive through OUR translation unit, where build_src_flags applies. Isolated at
// the include site exactly as SensorLib is in board/accel.cpp, rather than by
// dropping a flag project-wide: vendor warnings by the dozen are how a real
// -Wdouble-promotion in our own float math gets scrolled past.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#include <NimBLEDevice.h>
#pragma GCC diagnostic pop

#include "board/diag.h"
#include "core/protocol.h"

namespace board {
namespace ble {
namespace {

// ── PROTOCOL.md §2.2, advertising ────────────────────────────────────────────
//
// 0.625 ms units, so 160 == 100 ms. §8 reserves core/protocol.h for UUIDs, field
// offsets and the §5 timeouts; the advertising interval is none of those, it is
// the radio's own configuration, and it lives with the radio.
constexpr uint16_t kAdvertiseIntervalUnits = 160;

// §2.2's scan response. The advertisement itself has no room: flags (3 B) plus
// the complete list of 128-bit service UUIDs (18 B) is 21 of the 31 bytes
// available, and the UUID is the functional half — CDM filtering and the phone's
// pending autoConnect both work from the advertisement, never from the scan
// response. The name is diagnostic.
constexpr char kLocalName[] = "Workaday";

// §3: both payloads are 12 bytes and neither side negotiates the MTU, so 20 is
// everything the default 23-byte ATT MTU can carry. Declaring it as each
// characteristic's maximum makes that structural rather than hopeful — a longer
// write is refused by the ATT layer instead of reaching a buffer sized on trust.
// It is comfortably above 12, so §7.3's oversized-payload case (13 bytes) still
// reaches the decoder and still comes back as BadLength.
constexpr uint16_t kMaxPayloadLength = 20;

// How long hangUp() waits for the link to actually drop after asking the host to
// terminate it. A GAP terminate reaches the peer within a connection interval —
// tens of milliseconds — and the phone's supervision timeout would otherwise end
// the link seconds later with its alarm still sounding (PROTOCOL.md §4.1). Not a
// §5 number: the phone never sees it, it bounds one local wait, and it lives
// beside the effect it bounds like kMaxPayloadLength does. It is also a genuine
// un-fed interval and is kept well inside §5.1's 6 s.
constexpr uint32_t kHangUpWaitMs = 1000;

constexpr EventBits_t kBitConnected = 1u << 0;
constexpr EventBits_t kBitTimeWritten = 1u << 1;
constexpr EventBits_t kBitDisconnected = 1u << 2;
constexpr EventBits_t kBitFindWritten = 1u << 3;
constexpr EventBits_t kBitAbort = 1u << 4;
constexpr EventBits_t kBitFindSubscribed = 1u << 5;
constexpr EventBits_t kBitMenu = 1u << 6;
// What a sync window waits on, and what a find session waits on. Two masks
// because a wait that wakes on a bit its classifier never consumes returns
// instantly on every later call — the defect wait() describes below — and
// core::SyncWindow has no signal for a Find write, a Find subscription or a
// button. Any of those during a sync window is therefore latched, ignored, and
// cleared by the next constructor; PROTOCOL.md §3.3 says exactly that.
constexpr EventBits_t kBitSync = kBitConnected | kBitTimeWritten | kBitDisconnected;
constexpr EventBits_t kBitFind = kBitSync | kBitFindWritten | kBitAbort | kBitFindSubscribed | kBitMenu;
constexpr EventBits_t kBitAny = kBitFind;

// One window's worth of state, in static storage.
//
// File-static rather than a member of Session because the NimBLE callbacks are
// plain virtual overrides with no user pointer to carry a `this` through, and
// because Law 2 prefers static storage to anything the heap has to hand out on a
// path that runs every hour for months. Only one window can be open at a time —
// setup() is single-threaded and ends in deep sleep — so there is nothing to
// multiplex.
//
// The NimBLE host task fills `payload` and then sets a bit; the wake's own task
// waits on that bit and only then reads. The event group is the ordering: its
// set and wait both run under a portMUX critical section, and the two cores of an
// ESP32 share internal SRAM with no data cache between them, so there is no
// stale copy for the reader to find.
struct Window {
  EventGroupHandle_t events = nullptr;
  StaticEventGroup_t event_storage{};
  uint8_t payload[kMaxPayloadLength] = {};
  volatile uint16_t payload_length = 0;
  // The Find characteristic's last write, kept apart from Time's: a phone that
  // dismisses a search in the same breath as its Time write must not have one
  // frame overwrite the other before the caller has read both.
  uint8_t find_payload[kMaxPayloadLength] = {};
  volatile uint16_t find_payload_length = 0;
  // The live link, for hangUp(). BLE_HS_CONN_HANDLE_NONE when nothing is up.
  volatile uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
  NimBLECharacteristic* status = nullptr;
  // The Find characteristic, for notifyFind() — the watch's side of §3.3.
  NimBLECharacteristic* find = nullptr;
};

Window g_window;

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  // Only the two-argument overloads are implemented: NimBLE invokes both forms
  // for every event, so overriding both would report each one twice.
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    static_cast<void>(server);
    g_window.conn_handle = desc->conn_handle;
    xEventGroupSetBits(g_window.events, kBitConnected);
  }

  void onDisconnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    static_cast<void>(server);
    static_cast<void>(desc);
    g_window.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    xEventGroupSetBits(g_window.events, kBitDisconnected);
  }
};

// PROTOCOL.md §3.3. Same shape as TimeCallbacks and the same honesty about the
// length: what arrived, not what was copied, so core::decodeFindWrite() can
// reject a 5-byte frame as BadLength instead of reading four bytes of it.
class FindCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic) override {
    const NimBLEAttValue value = characteristic->getValue();
    const uint16_t length = value.length();
    const uint16_t copied = length < kMaxPayloadLength ? length : kMaxPayloadLength;
    if (copied != 0) {
      memcpy(g_window.find_payload, value.data(), copied);
    }
    g_window.find_payload_length = length;
    xEventGroupSetBits(g_window.events, kBitFindWritten);
  }

  // §4.1: the phone subscribes to Find after the flagged Status, and the watch
  // answers with the current mode — from the wake's task, on the FindSubscribed
  // event, never from here. Bit 0 of subValue is "notifications on"; an
  // unsubscribe (0) is nothing the session needs to hear about.
  void onSubscribe(NimBLECharacteristic* characteristic, ble_gap_conn_desc* desc,
                   uint16_t subValue) override {
    static_cast<void>(characteristic);
    static_cast<void>(desc);
    if ((subValue & 0x01) != 0) {
      xEventGroupSetBits(g_window.events, kBitFindSubscribed);
    }
  }
};

class TimeCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic) override {
    const NimBLEAttValue value = characteristic->getValue();
    const uint16_t length = value.length();
    const uint16_t copied = length < kMaxPayloadLength ? length : kMaxPayloadLength;
    if (copied != 0) {
      memcpy(g_window.payload, value.data(), copied);
    }
    // The length that arrived, not the length that was copied. A payload of any
    // size but 12 is BadLength (§7.3) and the decoder rejects it without reading
    // the buffer, so reporting the real figure is what makes that check honest.
    g_window.payload_length = length;
    xEventGroupSetBits(g_window.events, kBitTimeWritten);
  }
};

// Static, so nothing here is ever delete-d by the library. NimBLEServer will free
// its callbacks object if asked to; setCallbacks() below says not to, and
// NimBLECharacteristic never frees its own.
ServerCallbacks g_server_callbacks;
TimeCallbacks g_time_callbacks;
FindCallbacks g_find_callbacks;

}  // namespace

Session::Session(const uint8_t* status, size_t status_length) {
  opened_ms_ = millis();

  g_window.payload_length = 0;
  g_window.find_payload_length = 0;
  g_window.conn_handle = BLE_HS_CONN_HANDLE_NONE;
  g_window.status = nullptr;
  g_window.find = nullptr;
  if (g_window.events == nullptr) {
    g_window.events = xEventGroupCreateStatic(&g_window.event_storage);
  }
  if (g_window.events == nullptr) {
    WD_LOG("ble: no event group — skipping the window");
    return;
  }
  xEventGroupClearBits(g_window.events, kBitAny);

  // §2.1's CCCD on Status is the one line of the identity table this code cannot
  // simply pass through: NimBLE synthesises the descriptor from the NOTIFY
  // property and refuses to let one be created by hand, so there is no call to
  // hand the UUID to. It is checked instead, against core/protocol.h rather than
  // against NimBLE's own 0x2902 — the constant in protocol.h is the contract, the
  // one in the stack is an implementation detail, and §8 exists so the two cannot
  // quietly become different numbers. If they ever disagree the phone would
  // subscribe to a descriptor that is not there, and §4 makes the notification —
  // not the connect — the definition of a successful exchange, so the window
  // would be dead on arrival. Refuse it here and say why.
  if (NimBLEUUID(core::kCccdUuid) != NimBLEUUID(uint16_t(BLE_GATT_DSC_CLT_CFG_UUID16))) {
    WD_LOG("ble: CCCD uuid does not match the stack's — skipping the window");
    return;
  }

  // Nothing below asks for bonding, encryption or an MTU exchange, and that is
  // the whole of §2.3 and the second half of §3. NimBLE's defaults are already
  // what the contract wants — sm_bonding is 0 and no characteristic carries an
  // ENC or AUTHEN permission — so the correct implementation of "v1 has no
  // security" is to call none of the security API at all. Adding pairing "for
  // safety" is a deliberate v2 item (§9) and would bump PROTO_VERSION.
  NimBLEDevice::init(kLocalName);

  // §2.2: the address must be the public factory-derived one and stable across
  // reboots, because CDM association and the phone's pending autoConnect are both
  // keyed to the MAC. A resolvable-private or randomised address silently breaks
  // reconnection after a reboot and presents as "the app just stopped working".
  // NimBLEDevice::m_own_addr_type is BLE_OWN_ADDR_PUBLIC unless something asks
  // otherwise, so the requirement is met by not calling setOwnAddrType() —
  // recorded here because the absence of a call is not self-documenting.

  NimBLEServer* server = NimBLEDevice::createServer();
  if (server == nullptr) {
    WD_LOG("ble: createServer failed");
    return;
  }
  server->setCallbacks(&g_server_callbacks, /*deleteCallbacks=*/false);

  // Law 1, and easy to miss: NimBLE restarts advertising by itself the moment a
  // central disconnects. That would leave the radio running past the end of the
  // window and into the teardown, on the one path — the phone hanging up after a
  // successful exchange (§4) — that happens every time it works.
  server->advertiseOnDisconnect(false);

  NimBLEService* service = server->createService(core::kSyncServiceUuid);
  if (service == nullptr) {
    WD_LOG("ble: createService failed");
    return;
  }

  // §3.1 is a write **with response**; NIMBLE_PROPERTY::WRITE is exactly that,
  // and WRITE_NR would lose the phone's per-operation acknowledgement that §4's
  // one-operation-at-a-time queue is built on.
  NimBLECharacteristic* time_chr = service->createCharacteristic(
      core::kTimeCharacteristicUuid, NIMBLE_PROPERTY::WRITE, kMaxPayloadLength);
  if (time_chr == nullptr) {
    WD_LOG("ble: Time characteristic failed");
    return;
  }
  time_chr->setCallbacks(&g_time_callbacks);

  g_window.status = service->createCharacteristic(
      core::kStatusCharacteristicUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
      kMaxPayloadLength);
  if (g_window.status == nullptr) {
    WD_LOG("ble: Status characteristic failed");
    return;
  }
  // §3.2's read path: what the *previous* sync achieved, so the app's diagnostic
  // screen works without having to write the clock first.
  if (status != nullptr && status_length != 0 && status_length <= kMaxPayloadLength) {
    g_window.status->setValue(status, status_length);
  }

  // §3.3, present in every window and not only in a find session: the service's
  // shape is the contract, and a phone that discovers it once and caches the
  // profile must find the same characteristics next time. Write for the phone's
  // dismissal, notify for the watch's mode. A write or a subscription that
  // arrives outside a find session is latched and ignored — see kBitSync.
  g_window.find = service->createCharacteristic(
      core::kFindCharacteristicUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY,
      kMaxPayloadLength);
  if (g_window.find == nullptr) {
    WD_LOG("ble: Find characteristic failed");
    return;
  }
  g_window.find->setCallbacks(&g_find_callbacks);

  if (!service->start()) {
    WD_LOG("ble: service start failed");
    return;
  }

  // §2.2's payload, built explicitly rather than left to NimBLE's packing
  // heuristic. The heuristic moves the name between the advertisement and the
  // scan response depending on what fits, and "what fits" is one byte away from
  // pushing the service UUID out — which would break CDM filtering and
  // autoConnect silently, because the watch would still be advertising.
  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advertisement.setCompleteServices(NimBLEUUID(core::kSyncServiceUuid));

  NimBLEAdvertisementData scan_response;
  scan_response.setName(kLocalName);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (advertising == nullptr) {
    WD_LOG("ble: no advertising object");
    return;
  }
  advertising->setAdvertisementData(advertisement);
  advertising->setScanResponseData(scan_response);
  advertising->setScanResponse(true);
  // §2.2: 100 ms. Slow enough to matter for current, fast enough that a
  // low-duty-cycle background scan finds the watch inside its 6 s window.
  advertising->setMinInterval(kAdvertiseIntervalUnits);
  advertising->setMaxInterval(kAdvertiseIntervalUnits);
  // Connectable undirected (ADV_IND) is NimBLE's default for a peripheral and is
  // what §2.2 requires; left unset so there is one fewer thing to disagree with.

  if (!NimBLEDevice::startAdvertising()) {
    WD_LOG("ble: startAdvertising failed");
    return;
  }

  // The window is open from here, and §5.1's clocks run from this instant rather
  // than from the top of the constructor: the 6 s advertising timeout is time
  // spent advertising, not time spent bringing NimBLE up. The caller feeds the
  // watchdog immediately after this returns, so that phase is also a complete
  // un-fed interval and not the tail of a longer one.
  opened_ms_ = millis();
  ok_ = true;
  WD_LOG("ble: advertising");
}

Session::~Session() {
  // Every exit path lands here — an early return from the constructor above, a
  // caller's `return` in the middle of the window, an exception. That is the whole
  // point of the guard: the radio is the most expensive peripheral on this board to
  // leave running, and a manual teardown at the end of a function is skipped by
  // exactly the error path that would leave it on.
  //
  // ── What this destructor does NOT do any more, and what pays for it ─────────
  //
  // It used to call NimBLEDevice::deinit(true). That is a P1 panic on every window
  // and the mechanism is in the library, not here:
  //
  //   deinit -> nimble_port_stop() -> ble_hs_stop(), which moves
  //   ble_hs_enabled_state to STOPPING before it does anything else. From that
  //   instant ble_hs_timer_reset() (ble_hs.c:464) stops re-arming the host timer
  //   and starts *deleting* it —
  //
  //       if (!ble_hs_is_enabled()) {
  //           ble_npl_callout_stop(&ble_hs_timer);
  //           ble_npl_callout_deinit(&ble_hs_timer);   // xTimerDelete + free
  //       } else {
  //           rc = ble_npl_callout_reset(&ble_hs_timer, ticks);
  //       }
  //
  //   — while GAP is still calling ble_hs_timer_sched() on every event. So the
  //   host frees its own FreeRTOS timer mid-flight, and the priority-1 timer
  //   daemon later services a queued command against the freed control block:
  //   pxCallbackFunction reads 0, and prvProcessReceivedCommands calls it.
  //   Core 0, PC = 0, InstrFetchProhibited, on every teardown.
  //
  // No connection is needed for this — ble_hs_timer is the timer advertising
  // itself arms — which is why it reproduces on the plain 6 s advertise timeout.
  // Traced on hardware to between markers 2/4 and 3/4, i.e. inside deinit().
  //
  // **The radio is now powered down by deep sleep instead.** That is not a
  // weakening of the guarantee so much as a move: esp_deep_sleep_start() drops the
  // RF and digital domains at the hardware level, unconditionally, and Law 1 makes
  // board::power::deepSleep() the terminus of every path. The guarantee still sits
  // at a single choke point; it is just that one rather than this one. What it
  // costs is stated at the class level in ble.h and in docs/power-budget.md,
  // because it is a real cost and it belongs where a reader of the contract sees
  // it, not buried here.
  //
  // Why not disable the controller explicitly instead, and keep the guarantee
  // here? Because it cannot be shown to be sound, and an unsound controller
  // shutdown is worse than the defect it would fix:
  //
  //   * esp_nimble_hci_deinit() deletes vhci_send_sem and NULLs it, then frees the
  //     HCI mempools — and the host's own TX path does
  //     xSemaphoreTake(vhci_send_sem, ...) and os_memblock_get(&ble_hci_cmd_pool)
  //     (esp_nimble_hci.c:83, :150). With the host still enabled that is a null
  //     take and a freed-pool allocation. Provably unsound.
  //   * esp_bt_controller_disable() lives in libbt.a with no source shipped, and
  //     its header documents no precondition at all. The one precondition the
  //     controller API does document — on esp_bt_controller_deinit() — is to
  //     "disconnect all existing connections" first, and on the idle-timeout and
  //     cap endings a link is live and only ble_hs_stop() would close it. So the
  //     paths that most need an explicit shutdown are the ones that would break
  //     the only documented rule.
  //
  // Consequence worth knowing: on those two endings the link is not hung up, so
  // the phone sees a supervision timeout rather than a clean disconnect.
  // PROTOCOL.md §6.2 already handles that ("disconnect mid-exchange -> close(),
  // backoff one step, re-arm"), but it is slower for the phone than §6.1's normal
  // ending and it is a behaviour change, not a free win.
  //
  // ── teardown brackets, WORKADAY_DIAG only ──────────────────────────────────
  //
  // Kept after they earned their keep placing the panic. They cost nothing at
  // WORKADAY_DIAG=0 and this is the region where the next surprise will be.
  //
  // They are also the only visibility there is on the cost above: **the interval
  // between "teardown 4/4" and deepSleep()'s own "sleep:" line is exactly how long
  // the radio stayed powered after the window closed.** Today that is microseconds.
  // If it is ever milliseconds, someone has put work between the two and the
  // guarantee in ble.h has been broken — see the note there about what neither the
  // compiler nor a host test can hold.
  WD_LOG("ble: teardown 1/4 (initialized=%d)", NimBLEDevice::getInitialized() ? 1 : 0);
  if (NimBLEDevice::getInitialized()) {
    // Sound with the host enabled, and the one thing that actually stops the radio
    // transmitting: ble_hs_enabled_state stays ON, so ble_hs_timer_reset() takes
    // its re-arm branch and nothing is freed. Marker 2/4 printing 3 ms before the
    // panic, on the build that still called deinit(), is the evidence.
    NimBLEDevice::stopAdvertising();
    WD_LOG("ble: teardown 2/4 advertising stopped");
    // Where deinit() used to be. The stack is deliberately left up; the numbering
    // is kept so logs from before and after this change line up.
    WD_LOG("ble: teardown 3/4 stack left to deep sleep");
  }

  g_window.status = nullptr;
  g_window.find = nullptr;
  // The event group is deliberately NOT deleted, and that follows directly from
  // leaving the stack up. The NimBLE callbacks are still live and every one of them
  // ends in xEventGroupSetBits(g_window.events, ...) — a disconnect arriving after
  // this point would set bits on a deleted handle. It is statically allocated, so
  // deleting it never freed anything anyway; the constructor clears the bits before
  // each window, so reuse is clean. What the old delete bought was symmetry with a
  // teardown that no longer happens.
  WD_LOG("ble: teardown 4/4 done");
}

core::SyncWindowEvent Session::wait() {
  if (!ok_ || g_window.events == nullptr) {
    // No window ever opened, so there is nothing to wait for and never will be.
    // Capped rather than a special value: it is terminal, the caller already
    // handles it, and it cannot be mistaken for "keep going".
    return core::SyncWindowEvent::Capped;
  }

  // How long this step may block is core::SyncWindow's answer, not this file's:
  // it owns which §5.1 phase is running, when that phase started and how the cap
  // truncates it, and test_sync_window drives every reachable state of that
  // arithmetic. All this module contributes is the clock reading.
  const uint32_t wait_ms = window_.waitMs(millis() - opened_ms_);

  // The wait itself: blocked on an event group, woken by a NimBLE callback or by
  // the timeout, never spinning and never feeding anything.
  //
  // clearOnExit is pdFALSE so that a connect and a write arriving back to back
  // are both seen — the write must not overtake the connect and cost the caller
  // its first watchdog feed. That makes clearing this call's own bit a
  // *requirement* rather than tidiness, and it is done below.
  //
  // core::SyncWindow's promise that StillWaiting always leaves real time to block
  // on is only worth anything if a non-zero millisecond survives the conversion to
  // ticks. At 1000 Hz it does — one tick is one millisecond — and at anything
  // coarser pdMS_TO_TICKS() would floor short waits to zero, turning the caller's
  // "wait again" into a spin at radio current. The framework sets
  // CONFIG_FREERTOS_HZ=1000; this is here so that a change to it fails the build
  // rather than the watch.
  static_assert(configTICK_RATE_HZ >= 1000,
                "a sub-millisecond tick is required or short waits round to zero");
  // kBitSync, not kBitAny: a Find write or a Back press latched during a sync
  // window has no consumer in core::SyncWindow, and waking on it would make every
  // later wait return instantly — the spin documented below, arriving by a new
  // door.
  const EventBits_t bits = xEventGroupWaitBits(g_window.events, kBitSync, /*clearOnExit=*/pdFALSE,
                                               /*waitForAll=*/pdFALSE, pdMS_TO_TICKS(wait_ms));

  core::SyncWindowSignals signals;
  signals.connected = (bits & kBitConnected) != 0;
  signals.time_written = (bits & kBitTimeWritten) != 0;
  signals.disconnected = (bits & kBitDisconnected) != 0;

  const core::SyncWindowEvent event = window_.classify(millis() - opened_ms_, signals);

  // **Every bit this call consumed has to be cleared here.** The wait above does
  // not auto-clear, so a bit that is reported to the caller and left set is still
  // set when the next xEventGroupWaitBits() runs — and that call returns
  // instantly, however long a timeout it was given. The wait stops being a wait.
  //
  // An earlier version of this cleared only kBitTimeWritten and argued that the
  // connect "needs no clearing (the window latches it)". The latch is real and it
  // is the wrong mechanism: it stops core::SyncWindow reporting a second
  // `Connected` *event*, and does nothing whatever about xEventGroupWaitBits
  // returning on a bit that is still set. On hardware that read as
  //
  //     w: el=429  bits=01 ev=0     connected, correct
  //     w: el=429  wait=4000        the 4 s idle budget, correct
  //     w: el=429  bits=01 ev=3     returned instantly, same bit, TimedOut
  //
  // — the watch connected and tore down in the same millisecond, so the phone
  // never got its write window and a sync could not complete. PROTOCOL.md §5.1's
  // two phases existed only on paper.
  //
  // The rule is therefore mechanical rather than case-by-case: a bit turned into
  // a non-terminal event is spent and gets cleared. Terminal events do not need
  // it — the window is latched, waitMs() is 0 and the caller must stop — but
  // anything added to kBitAny later must be consumed on this switch, or it
  // reintroduces exactly the defect above.
  //
  // §4 permits a second Time write in the same connection, so the write bit is
  // cleared to make the *next* one a fresh event rather than a leftover; the
  // connect bit is cleared because it is level and there is only ever one connect
  // in a window (advertiseOnDisconnect is off and a disconnect is terminal).
  switch (event) {
    case core::SyncWindowEvent::Connected:
      xEventGroupClearBits(g_window.events, kBitConnected);
      break;
    case core::SyncWindowEvent::TimeWritten:
      xEventGroupClearBits(g_window.events, kBitTimeWritten);
      break;
    default:
      break;
  }
  return event;
}

void Session::noteWriteResult(core::SyncResult result) { window_.noteWriteResult(result); }

size_t Session::copyTimeWrite(uint8_t* out, size_t cap) const {
  const size_t length = g_window.payload_length;
  if (out != nullptr) {
    const size_t copied = length < cap ? length : cap;
    const size_t available = copied < sizeof(g_window.payload) ? copied
                                                               : sizeof(g_window.payload);
    if (available != 0) {
      memcpy(out, g_window.payload, available);
    }
  }
  return length;
}

bool Session::notify(const uint8_t* status, size_t length) {
  if (!ok_ || g_window.status == nullptr || status == nullptr || length == 0 ||
      length > kMaxPayloadLength) {
    return false;
  }
  // setValue as well as notify: §3.2's read path must report the outcome the
  // notification carried, not the state the window started with, if the phone
  // reads instead of subscribing.
  g_window.status->setValue(status, length);
  const bool subscribed = g_window.status->getSubscribedCount() != 0;
  g_window.status->notify(status, length);
  return subscribed;
}

// ── the find-phone session ───────────────────────────────────────────────────

uint32_t Session::elapsedMs() const { return millis() - opened_ms_; }

core::FindEvent Session::findWait(core::FindSession& session, bool (*menu_held)()) {
  if (!ok_ || g_window.events == nullptr) {
    // No window ever opened. The caller checked ok() and never gets here; if it
    // did, the honest answer is that the session is over before it started.
    return core::FindEvent::Ended;
  }

  // The same shape as wait(), with core::FindSession owning the arithmetic: how
  // long this round may block, and what the pending bits mean. All seven bits
  // this time — the ones the sync wait deliberately ignores are exactly the ones
  // that end or steer a search.
  const uint32_t wait_ms = session.waitMs(elapsedMs());
  const EventBits_t bits = xEventGroupWaitBits(g_window.events, kBitFind, /*clearOnExit=*/pdFALSE,
                                               /*waitForAll=*/pdFALSE, pdMS_TO_TICKS(wait_ms));

  core::FindSignals signals;
  signals.connected = (bits & kBitConnected) != 0;
  signals.time_written = (bits & kBitTimeWritten) != 0;
  signals.disconnected = (bits & kBitDisconnected) != 0;
  signals.find_written = (bits & kBitFindWritten) != 0;
  signals.back_pressed = (bits & kBitAbort) != 0;
  signals.find_subscribed = (bits & kBitFindSubscribed) != 0;
  signals.menu_edge = (bits & kBitMenu) != 0;
  // Sampled now, after the wait, which is the only moment it means anything: a
  // press is confirmed kFindButtonSettleMs after its edge by the pin still being
  // high, and the wait was cut to exactly that deadline. One GPIO read per wake
  // of this loop, not a poll — the loop wakes on events and deadlines.
  signals.menu_held = menu_held != nullptr && menu_held();

  const core::FindEvent event = session.classify(elapsedMs(), signals);

  // The same mechanical rule as wait(): a bit turned into a non-terminal event is
  // spent and is cleared here, or the next call returns instantly on it. The
  // disconnect is cleared on Disconnected whether or not core believed a link was
  // up — core::FindSession consumes it either way for exactly this reason. Ended
  // is latched and waitMs() is zero, so the terminal bits need no clearing.
  switch (event) {
    case core::FindEvent::Connected:
      xEventGroupClearBits(g_window.events, kBitConnected);
      break;
    case core::FindEvent::TimeWritten:
      xEventGroupClearBits(g_window.events, kBitTimeWritten);
      break;
    case core::FindEvent::FindWritten:
      xEventGroupClearBits(g_window.events, kBitFindWritten);
      break;
    case core::FindEvent::Disconnected:
      xEventGroupClearBits(g_window.events, kBitDisconnected);
      break;
    case core::FindEvent::FindSubscribed:
      xEventGroupClearBits(g_window.events, kBitFindSubscribed);
      break;
    case core::FindEvent::SoundToggled:
    case core::FindEvent::RoundElapsed:
    case core::FindEvent::Ended:
    case core::FindEvent::StillWaiting:
      break;
  }
  // The Menu edge is the one bit not tied to an event: core::FindSession records
  // it whatever it returns — it only starts a settle clock, or is dropped as a
  // bounce or as a press with nobody on the link — so it is spent on every call
  // that saw it. Left set it would be the instant-return spin above.
  if (signals.menu_edge) {
    xEventGroupClearBits(g_window.events, kBitMenu);
  }
  return event;
}

bool Session::notifyFind(const uint8_t* frame, size_t length) {
  if (!ok_ || g_window.find == nullptr || frame == nullptr || length == 0 ||
      length > kMaxPayloadLength) {
    return false;
  }
  // setValue as well as notify, as for Status: a phone that reads Find gets the
  // current mode rather than whatever the characteristic held at creation.
  g_window.find->setValue(frame, length);
  const bool subscribed = g_window.find->getSubscribedCount() != 0;
  // With no subscriber NimBLE sends nothing, which is the §6.1 no-op — a mode
  // change before the phone subscribed is delivered on FindSubscribed instead.
  g_window.find->notify(frame, length);
  return subscribed;
}

size_t Session::copyFindWrite(uint8_t* out, size_t cap) const {
  const size_t length = g_window.find_payload_length;
  if (out != nullptr) {
    const size_t copied = length < cap ? length : cap;
    const size_t available = copied < sizeof(g_window.find_payload)
                                 ? copied
                                 : sizeof(g_window.find_payload);
    if (available != 0) {
      memcpy(out, g_window.find_payload, available);
    }
  }
  return length;
}

bool Session::restartAdvertising() {
  if (!ok_) {
    return false;
  }
  // The advertisement data, interval and scan response set in the constructor are
  // still in the advertising object; only the "start" has to be repeated.
  return NimBLEDevice::startAdvertising();
}

bool Session::hangUp() {
  if (!ok_ || g_window.events == nullptr) {
    return false;
  }
  NimBLEServer* server = NimBLEDevice::getServer();
  const uint16_t handle = g_window.conn_handle;
  if (server == nullptr || handle == BLE_HS_CONN_HANDLE_NONE) {
    return true;  // nothing to hang up
  }
  // ble_gap_terminate() through the library: a plain host call with the host
  // enabled, which is sound — the panic ble.h describes belongs to deinit(), which
  // moves the host out of its enabled state, and nothing here does that.
  xEventGroupClearBits(g_window.events, kBitDisconnected);
  const int rc = server->disconnect(handle);
  if (rc != 0) {
    // Already gone, most likely — the peer dropped between the caller's last
    // classify() and now. Either way there is no link left to wait for.
    WD_LOG("ble: hang-up rc=%d", rc);
    return g_window.conn_handle == BLE_HS_CONN_HANDLE_NONE;
  }
  // Blocked on the disconnect event, never polled, and bounded by kHangUpWaitMs
  // so a peer that never acknowledges cannot hold the wake past the watchdog.
  const EventBits_t bits = xEventGroupWaitBits(g_window.events, kBitDisconnected, /*clearOnExit=*/pdTRUE,
                                               /*waitForAll=*/pdFALSE, pdMS_TO_TICKS(kHangUpWaitMs));
  return (bits & kBitDisconnected) != 0;
}

// IRAM: this runs from the GPIO interrupt board::buttons attaches for the length
// of a find session, and an ISR that lives in flash faults if it fires while the
// cache is disabled. xEventGroupSetBitsFromISR() defers the actual set to the
// FreeRTOS timer daemon — the FromISR-safe half of the event-group API, and the
// framework builds with INCLUDE_xTimerPendFunctionCall, which it requires.
IRAM_ATTR void Session::requestAbort() {
  if (g_window.events == nullptr) {
    return;
  }
  BaseType_t woken = pdFALSE;
  xEventGroupSetBitsFromISR(g_window.events, kBitAbort, &woken);
  if (woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// Same shape and the same IRAM reasoning as requestAbort(). This one only reports
// an edge; whether it was a press is core::FindSession's to decide from the pin
// level kFindButtonSettleMs later, which is why bouncing here costs nothing.
IRAM_ATTR void Session::requestSoundToggle() {
  if (g_window.events == nullptr) {
    return;
  }
  BaseType_t woken = pdFALSE;
  xEventGroupSetBitsFromISR(g_window.events, kBitMenu, &woken);
  if (woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

}  // namespace ble
}  // namespace board
