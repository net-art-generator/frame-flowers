// © Kay Sievers <kay@versioduo.com>, 2023
// SPDX-License-Identifier: Apache-2.0

#include "Pictures.h"
#include <V2Base.h>
#include <V2Buttons.h>
#include <V2Color.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2MIDI.h>
#include <V2Music.h>

V2DEVICE_METADATA("com.versioduo.frame-flowers", 1, "versioduo:samd:strip");

static V2LED::WS2812 LED(2, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2LED::WS2812 LEDExt(256, PIN_LED_WS2812_EXT, &sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);

// Config, written to EEPROM.
static constexpr struct Configuration {
  struct {
    uint8_t orientation{};
    bool mirror{};
    float power{0.5};
  } led;

  struct {
    uint32_t sleepSec{30};
  } play;
} ConfigurationDefault{};

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 frame-flowers";
    metadata.description = "16 x 16 LED Matrix Display";
    metadata.home        = "https://versioduo.com/#frame-flowers";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid            = 0xe9c0;
    usb.ports.standard = 0;

    configuration = {.size{sizeof(config)}, .data{&config}};
  }

  Configuration config{ConfigurationDefault};

  void stop() {
    _state = State::Stop;
  }

  void showPicture(const uint8_t *picture, float brightness = 1) {
    for (uint8_t y = 0; y < 16; y++) {
      for (uint8_t x = 0; x < 16; x++) {
        const uint8_t *pixel = &picture[(y * 16 * 3) + (x * 3)];
        setLED(x, y, pixel[0], pixel[1], pixel[2], brightness);
      }
    }
  }

private:
  enum class CC {
    Manual = V2MIDI::CC::ModulationWheel,
  };

  enum class State { Sleep, Pictures, Stop } _state{};
  uint32_t _usec{};
  struct {
    uint8_t control;
    uint8_t velocity;
    uint8_t note;
  } _manual;

  struct {
    uint16_t sequence[2]{};
    uint16_t next{};
    bool blend{};
  } _pictures;

  void handleReset() override {
    _state    = {};
    _usec     = V2Base::getUsec();
    _manual   = {};
    _pictures = {};

    LED.reset();
    LED.setHSV(V2Color::Orange, 0.8, 0.15);
    LEDExt.reset();
    setMaxBrightness();
  }

  void setLED(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b, float brightness) {
    if (config.led.mirror)
      x = 15 - x;

    switch (config.led.orientation) {
      case 1: {
        const uint8_t t = x;
        x               = 15 - y;
        y               = t;
      } break;

      case 2:
        x = 15 - x;
        y = 15 - y;
        break;

      case 3: {
        const uint8_t t = x;
        x               = y;
        y               = 15 - t;
      } break;
    }

    // The LEDs are in a zigzag pattern.
    const uint8_t xReversed = (y % 2 == 0) ? x : 15 - x;
    r                       = (float)r * brightness;
    g                       = (float)g * 0.9f * brightness;
    b                       = (float)b * 0.8f * brightness;
    LEDExt.setRGB(y * 16 + xReversed, r, g, b);
  }

  // Return fraction, it does not reach 1.
  float getRandom() {
    const uint32_t random = V2Base::Cryptography::Random::read();
    return (float)(random - 1) / (float)UINT32_MAX;
  }

  void blendPicture(const uint8_t *fromPicture, const uint8_t *picture, float brightness = 1) {
    for (uint8_t y = 0; y < 16; y++) {
      for (uint8_t x = 0; x < 16; x++) {
        const uint8_t *fromPixel = &fromPicture[(y * 16 * 3) + (x * 3)];
        const uint8_t *pixel     = &picture[(y * 16 * 3) + (x * 3)];
        const uint8_t r          = (pixel[0] + fromPixel[0]) / 2;
        const uint8_t g          = (pixel[1] + fromPixel[1]) / 2;
        const uint8_t b          = (pixel[2] + fromPixel[2]) / 2;
        setLED(x, y, r, g, b, brightness);
      }
    }
  }

  void handleLoop() override {
    switch (_state) {
      case State::Sleep:
        if (V2Base::getUsecSince(_usec) < config.play.sleepSec * 1000 * 1000)
          return;

        // Randomly select two unique pictures as a sequence.
        _state                = State::Pictures;
        _pictures.sequence[0] = getRandom() * (V2Base::countof(Pictures));

        for (;;) {
          _pictures.sequence[1] = getRandom() * (V2Base::countof(Pictures));
          if (_pictures.sequence[1] != _pictures.sequence[0])
            break;
        }

        _pictures.next  = 0;
        _pictures.blend = false;
        break;

      case State::Pictures:
        if (V2Base::getUsecSince(_usec) < 2000 * 1000)
          return;

        if (_pictures.next == V2Base::countof(_pictures.sequence)) {
          LEDExt.reset();
          _state = State::Sleep;
          break;
        }

        if (_pictures.blend) {
          blendPicture(Pictures[_pictures.sequence[_pictures.next - 1]], Pictures[_pictures.sequence[_pictures.next]]);
          _pictures.blend = false;

        } else {
          showPicture(Pictures[_pictures.sequence[_pictures.next]]);
          _pictures.next++;
          _pictures.blend = true;
        }
        break;

      case State::Stop:
        return;
    }

    _usec = V2Base::getUsec();
  }

  void handleSystemReset() override {
    reset();
  }

  void setMaxBrightness() {
    const float min   = 0.05;
    const float max   = 0.4;
    const float range = (max - min) * config.led.power;
    LEDExt.setMaxBrightness(min + range);
  }

  void updateManual() {
    if (_manual.control == 0 && _manual.velocity == 0) {
      reset();
      return;
    }

    LEDExt.rainbow(0);
    stop();

    if (_manual.velocity > 0) {
      const uint8_t notePicture = _manual.note - V2MIDI::C(3);
      const float fraction      = (float)_manual.velocity / 127.f;
      const float brightness    = 0.2f + (0.8f * fraction);

      if (_manual.control > 0)
        blendPicture(Pictures[_manual.control - 1], Pictures[notePicture], brightness);

      else
        showPicture(Pictures[notePicture], brightness);
      return;
    }

    showPicture(Pictures[_manual.control - 1]);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    if (channel != 0)
      return;

    switch (controller) {
      case (uint8_t)CC::Manual:
        if (value > V2Base::countof(Pictures))
          break;

        _manual.control = value;
        updateManual();
        break;

      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        reset();
    }
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    if (channel != 0)
      return;

    if (note < V2MIDI::C(3))
      return;

    if (note >= V2MIDI::C(3) + V2Base::countof(Pictures))
      return;

    _manual.note     = note;
    _manual.velocity = velocity;
    updateManual();
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    handleNote(channel, note, 0);
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.createNestedObject();
      setting["title"]   = "LED";
      setting["type"]    = "number";
      setting["label"]   = "Power";
      setting["min"]     = 0;
      setting["max"]     = 1;
      setting["step"]    = 0.01;
      setting["default"] = ConfigurationDefault.led.power;
      setting["path"]    = "led/power";
    }
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "toggle";
      setting["label"]   = "Mirror";
      setting["path"]    = "led/mirror";
    }
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "number";
      setting["label"]   = "Orientation";
      setting["min"]     = 0;
      setting["max"]     = 3;
      setting["input"]   = "select";
      setting["default"] = ConfigurationDefault.led.orientation;
      setting["path"]    = "led/orientation";
      JsonArray names    = setting.createNestedArray("names");
      names.add("0°");
      names.add("90°");
      names.add("180°");
      names.add("270°");
    }
    {
      JsonObject setting = json.createNestedObject();
      setting["title"]   = "Play";
      setting["type"]    = "number";
      setting["label"]   = "Sleep";
      setting["text"]    = "Seconds";
      setting["min"]     = 10;
      setting["max"]     = 1000;
      setting["step"]    = 10;
      setting["default"] = ConfigurationDefault.play.sleepSec;
      setting["path"]    = "play/sleep";
    }
  }

  void importConfiguration(JsonObject json) override {
    if (!json["led"].isNull()) {
      JsonObject led = json["led"];

      if (!led["orientation"].isNull()) {
        config.led.orientation = led["orientation"];
        if (config.led.orientation > 3)
          config.led.orientation = 3;
      }

      if (!led["mirror"].isNull())
        config.led.mirror = led["mirror"];

      if (!led["power"].isNull()) {
        config.led.power = led["power"];
        if (config.led.power < 0.f)
          config.led.power = 0;

        if (config.led.power > 1.f)
          config.led.power = 1;
      }
    }

    if (!json["play"].isNull()) {
      JsonObject play = json["play"];

      if (!play["sleep"].isNull()) {
        config.play.sleepSec = play["sleep"];
        if (config.play.sleepSec < 10)
          config.play.sleepSec = 10;

        if (config.play.sleepSec > 1000)
          config.play.sleepSec = 1000;
      }
    }

    setMaxBrightness();
  }

  void exportInput(JsonObject json) override {
    JsonArray jsonControllers = json.createNestedArray("controllers");
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Picture";
      jsonController["number"]  = (uint8_t)CC::Manual;
      jsonController["value"]   = _manual.control;
      jsonController["max"]     = V2Base::countof(Pictures);
    }

    JsonObject jsonChromatic = json.createNestedObject("chromatic");
    jsonChromatic["start"]   = V2MIDI::C(3);
    jsonChromatic["count"]   = V2Base::countof(Pictures);
  }

  void exportConfiguration(JsonObject json) override {
    JsonObject jsonLED      = json.createNestedObject("led");
    jsonLED["#orientation"] = "Rotate the picture in 90 degree steps (0..3)";
    jsonLED["orientation"]  = config.led.orientation;
    jsonLED["#mirror"]      = "Mirror the picture";
    jsonLED["mirror"]       = config.led.mirror;
    jsonLED["#power"]       = "The maximum brightness of the LEDs (0..1)";
    jsonLED["power"]        = serialized(String(config.led.power, 2));

    JsonObject jsonPlay = json.createNestedObject("play");
    jsonPlay["#sleep"]  = "The number of seconds between the pictures (10..1000)";
    jsonPlay["sleep"]   = config.play.sleepSec;
  }
} Device;

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(&_midi))
      return;

    if (_midi.getPort() != 0)
      return;

    Device.dispatch(&Device.usb.midi, &_midi);
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

static class Button : public V2Buttons::Button {
public:
  Button() : V2Buttons::Button(&_config, PIN_BUTTON) {}

private:
  const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

  void handleClick(uint8_t count) override {
    Device.reset();
  }

  void handleHold(uint8_t count) override {
    switch (count) {
      case 0:
        Device.stop();
        LED.setHSV(V2Color::Cyan, 0.8, 0.15);
        LEDExt.rainbow(1, 3, 0.75);
        break;

      case 1 ... V2Base::countof(Pictures):
        LEDExt.rainbow(0);
        Device.stop();
        Device.showPicture(Pictures[count - 1]);
        break;
    }
  }
} Button;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LEDExt.begin();

  Button.begin();
  Device.begin();
  Device.reset();
}

void loop() {
  LED.loop();
  LEDExt.loop();
  MIDI.loop();
  V2Buttons::loop();
  Device.loop();

  if (Device.idle())
    Device.sleep();
}
