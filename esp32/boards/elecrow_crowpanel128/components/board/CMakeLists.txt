# The Elecrow CrowPanel 1.28" rotary's `board` component: the shared board interface
# (board/board.h) plus board/knob.h (the encoder, its switch and the LED ring), with small
# drivers (GC9A01 over esp_lcd SPI, CST816D over I2C, PCNT quadrature, WS2812 over RMT,
# LEDC backlight).
idf_component_register(SRCS src/board.cpp
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio
                                     esp_driver_ledc esp_driver_pcnt esp_driver_rmt
                                     esp_driver_usb_serial_jtag esp_timer mbedtls)
