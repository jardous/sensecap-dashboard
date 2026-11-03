#pragma once
#include "RadioLib.h"
#include "board.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Native S3 SPI/GPIO APIs; does not copy RadioLib's ESP32-only register HAL.
class EspHal final : public RadioLibHal {
public:
    EspHal():RadioLibHal(GPIO_MODE_INPUT,GPIO_MODE_OUTPUT,0,1,GPIO_INTR_POSEDGE,GPIO_INTR_NEGEDGE) {}
    void init() override { spiBegin(); }
    void pinMode(uint32_t pin,uint32_t mode) override {
        if (pin==RADIOLIB_NC) return;
        gpio_config_t c{}; c.pin_bit_mask=1ull<<pin; c.mode=gpio_mode_t(mode);
        ESP_ERROR_CHECK(gpio_config(&c));
    }
    void digitalWrite(uint32_t pin,uint32_t level) override { if (pin!=RADIOLIB_NC) gpio_set_level(gpio_num_t(pin),level); }
    uint32_t digitalRead(uint32_t pin) override { return pin==RADIOLIB_NC?0:gpio_get_level(gpio_num_t(pin)); }
    void attachInterrupt(uint32_t pin,void(*cb)(),uint32_t mode) override {
        callback_=cb;
        const auto rc=gpio_install_isr_service(0); if (rc!=ESP_OK && rc!=ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(rc);
        ESP_ERROR_CHECK(gpio_set_intr_type(gpio_num_t(pin),gpio_int_type_t(mode)));
        ESP_ERROR_CHECK(gpio_isr_handler_add(gpio_num_t(pin),isr,this));
    }
    void detachInterrupt(uint32_t pin) override { gpio_isr_handler_remove(gpio_num_t(pin)); }
    void delay(RadioLibTime_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)?pdMS_TO_TICKS(ms):1); }
    void delayMicroseconds(RadioLibTime_t us) override { esp_rom_delay_us(us); }
    RadioLibTime_t millis() override { return esp_timer_get_time()/1000; }
    RadioLibTime_t micros() override { return esp_timer_get_time(); }
    long pulseIn(uint32_t,uint32_t,RadioLibTime_t) override { return 0; } // unused by SX1262 RX
    void yield() override { taskYIELD(); }
    void spiBegin() override {
        if (spi_) return;
        spi_bus_config_t bus{}; bus.mosi_io_num=board::mosi; bus.miso_io_num=board::miso; bus.sclk_io_num=board::sck;
        bus.quadwp_io_num=-1; bus.quadhd_io_num=-1; bus.max_transfer_sz=512;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
        spi_device_interface_config_t dev{}; dev.clock_speed_hz=2000000; dev.mode=0; dev.spics_io_num=-1; dev.queue_size=1;
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST,&dev,&spi_));
    }
    void spiBeginTransaction() override { first_=true; ESP_ERROR_CHECK(spi_device_acquire_bus(spi_,portMAX_DELAY)); }
    void spiTransfer(uint8_t* out,size_t n,uint8_t* in) override {
        if (!n) return;
        if (first_) {
            first_=false;
            // Independent fail-closed hardware boundary: never put any SX1262
            // SetTx, SetTxContinuousWave or SetTxInfinitePreamble opcode on SPI.
            if (out[0]==0x83 || out[0]==0xD1 || out[0]==0xD2) {
                ESP_LOGE("rx_guard","Blocked forbidden radio command"); abort();
            }
        }
        spi_transaction_t t{}; t.length=n*8; t.tx_buffer=out; t.rx_buffer=in;
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi_,&t));
    }
    void spiEndTransaction() override { spi_device_release_bus(spi_); }
    void spiEnd() override {}
private:
    static void isr(void* arg) { const auto* self=static_cast<EspHal*>(arg); if (self->callback_) self->callback_(); }
    spi_device_handle_t spi_=nullptr;
    void (*callback_)()=nullptr;
    bool first_=true;
};
