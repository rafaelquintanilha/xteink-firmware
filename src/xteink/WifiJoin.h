#pragma once
// Unattended Wi-Fi join: try the last-connected SSID first, then scan and try
// every other saved network that is in range, strongest signal first, all
// inside one hard deadline. The strategy is pure logic behind WifiPort so it is
// unit-tested on the host (test/xteink_wifijoin); the Arduino adapter is
// compiled only on the device.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xteink::wifijoin {

struct ScanResult {
  std::string ssid;
  int rssi = 0;
  int32_t channel = 0;  // 0 = unknown
};

struct Credential {
  std::string ssid;
  std::string password;
};

enum class Error : uint8_t {
  None,
  NoSavedNetworks,  // nothing to try
  NoneInRange,      // scan finished, no saved network visible
  JoinFailed,       // every candidate attempt failed
  Timeout,          // the hard deadline expired
};

struct Result {
  Error error = Error::None;
  std::string ssid;  // joined SSID when error == None
  bool ok() const { return error == Error::None; }
};

// Everything the strategy needs from the outside world.
class WifiPort {
 public:
  virtual ~WifiPort() = default;
  virtual std::string lastConnectedSsid() = 0;
  virtual std::optional<Credential> findCredential(const std::string& ssid) = 0;
  virtual size_t savedCount() = 0;
  // Blocking scan, bounded by the port to at most maxMs.
  virtual std::vector<ScanResult> scan(uint32_t maxMs) = 0;
  // `channel` is where the scan saw the network (0 = unknown, the driver scans
  // for it); a known channel lets the connect skip its own scan.
  virtual void begin(const std::string& ssid, const std::string& password, int32_t channel) = 0;
  virtual bool connected() = 0;
  virtual void disconnect() = 0;
  virtual void sleepMs(uint32_t ms) = 0;
  virtual uint32_t millis() = 0;
};

constexpr uint32_t kPerNetworkTimeoutMs = 8000;
constexpr uint32_t kPollMs = 100;

const char* errorName(Error e);

// Tries to join within deadlineMs (a budget measured from the call, not an
// absolute time). Returns the joined SSID or a failure code.
//
// `scanFirst` skips the blind attempt on the last-connected SSID: the scan runs
// first and only saved networks it found are tried (the last-connected one
// first, then by signal). With nothing in range this fails after one scan
// instead of after kPerNetworkTimeoutMs plus a scan. A hidden network is never
// found by a scan, so it is not joined in this mode.
Result joinAnySaved(WifiPort& port, uint32_t deadlineMs, bool scanFirst = false);

#ifdef ARDUINO
// Device entry point: adapts Arduino WiFi + WIFI_STORE; records the joined SSID
// as last-connected (not persisted to disk here).
Result joinAnySaved(uint32_t deadlineMs, bool scanFirst = false);
#endif

}  // namespace xteink::wifijoin
