# Patched dependencies (ESP-IDF 6.x)

Local copies of registry components, wired in with `override_path` in `main/idf_component.yml` (folder names match the
registry component names, e.g. `espressif__esp-dsp`, because other components look them up by that name)
because the pinned upstream versions do not build on ESP-IDF 6.x. Example/test folders were dropped.

- `espressif__esp_codec_dev` (1.4.0, pinned by livekit -> av_render ~1.4): CMakeLists REQUIRES now also lists
  esp_driver_gpio, esp_driver_i2c, esp_driver_i2s and esp_driver_spi; on IDF 6 `driver` no longer pulls them in.
- `espressif__esp-dsp` (1.6.0, pinned by esp-sr): added `#include <cmath>` to kalman/ekf/common/ekf.cpp and
  kalman/ekf_imu13states/ekf_imu13states.cpp (GCC 15 libstdc++ no longer pulls it in transitively).
- `espressif__esp_codec_dev` also: platform/audio_codec_ctrl_spi.c includes `esp_idf_version.h` (no longer pulled in by the SPI headers).
- `tempotian__media_lib_sal` (pinned by livekit/esp_peer): on IDF >= 6, port/media_lib_crypt_default.c registers no default
  md5/sha256/aes adapter (mbedtls 4 removed md5.h and the `*_ret` APIs). Nothing in this firmware calls media_lib's crypto.
