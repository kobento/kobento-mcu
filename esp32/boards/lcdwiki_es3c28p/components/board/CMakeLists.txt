# The LCDWIKI ES3C28P (2.8" ESP32-S3 display) `board` component: the shared board interface
# (board/board.h) with small drivers (ILI9341V over esp_lcd SPI, FT6336G over I2C, LEDC
# backlight, the BOOT key), and audio.cpp: a Teensy Audio Shield (SGTL5000) on I2S.
idf_component_register(SRCS src/board.cpp src/audio.cpp src/speaker.cpp
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio
                                     esp_driver_ledc esp_driver_i2s esp_driver_usb_serial_jtag esp_timer mbedtls)
