/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "radio.hpp"

#include "rf_path.hpp"

#include "rffc507x.hpp"
#include "max2837.hpp"
#include "max2839.hpp"
#ifdef PRALINE
#include "max2831.hpp"
#endif
#include "max5864.hpp"
#include "baseband_cpld.hpp"

#include "tuning.hpp"

#include "spi_arbiter.hpp"

#include "hackrf_hal.hpp"
#include "hackrf_gpio.hpp"
using namespace hackrf::one;

#include "cpld_update.hpp"

#include "portapack.hpp"
#include "portapack_persistent_memory.hpp"

/* Direct access to the radio. Setting values incorrectly can damage
 * the device. Applications should use ReceiverModel or TransmitterModel
 * instead of calling these functions directly. */
namespace radio {

static constexpr uint32_t ssp1_cpsr = 2;

static constexpr uint32_t ssp_scr(
    const float pclk_f,
    const uint32_t cpsr,
    const float spi_f) {
    return static_cast<uint8_t>(pclk_f / cpsr / spi_f - 1);
}

#ifdef PRALINE
/* MAX2831 uses 9-bit SPI transfers */
static constexpr SPIConfig ssp_config_max283x = {
    .end_cb = NULL,
    .ssport = gpio_max283x_select.port(),
    .sspad = gpio_max283x_select.pad(),
    .cr0 =
        CR0_CLOCKRATE(ssp_scr(ssp1_pclk_f, ssp1_cpsr, max283x_spi_f) + 3) | CR0_FRFSPI | CR0_DSS9BIT,
    .cpsr = ssp1_cpsr,
};
#else
/* MAX2837/MAX2839 use 16-bit SPI transfers */
static constexpr SPIConfig ssp_config_max283x = {
    .end_cb = NULL,
    .ssport = gpio_max283x_select.port(),
    .sspad = gpio_max283x_select.pad(),
    .cr0 =
        CR0_CLOCKRATE(ssp_scr(ssp1_pclk_f, ssp1_cpsr, max283x_spi_f) + 3) | CR0_FRFSPI | CR0_DSS16BIT,
    .cpsr = ssp1_cpsr,
};
#endif

static constexpr SPIConfig ssp_config_max5864 = {
    .end_cb = NULL,
    .ssport = gpio_max5864_select.port(),
    .sspad = gpio_max5864_select.pad(),
    .cr0 =
        CR0_CLOCKRATE(ssp_scr(ssp1_pclk_f, ssp1_cpsr, max5864_spi_f)) | CR0_FRFSPI | CR0_DSS8BIT,
    .cpsr = ssp1_cpsr,
};

#ifdef PRALINE
/* FPGA (iCE40) uses 8-bit SPI Mode 3 (CPOL=1, CPHA=1) */
static constexpr SPIConfig ssp_config_fpga = {
    .end_cb = NULL,
    .ssport = gpio_fpga_select.port(),
    .sspad = gpio_fpga_select.pad(),
    .cr0 =
        CR0_CLOCKRATE(ssp_scr(ssp1_pclk_f, ssp1_cpsr, max5864_spi_f)) | CR0_FRFSPI | CR0_DSS8BIT | CR0_CPOL | CR0_CPHA,
    .cpsr = ssp1_cpsr,
};
#endif

static spi::arbiter::Arbiter ssp1_arbiter(portapack::ssp1);

static spi::arbiter::Target ssp1_target_max283x{
    ssp1_arbiter,
    ssp_config_max283x};

static spi::arbiter::Target ssp1_target_max5864{
    ssp1_arbiter,
    ssp_config_max5864};

#ifdef PRALINE
static spi::arbiter::Target ssp1_target_fpga{
    ssp1_arbiter,
    ssp_config_fpga};
#endif

static rf::path::Path rf_path;
rffc507x::RFFC507x first_if;
max283x::MAX283x* second_if;
max2837::MAX2837 second_if_max2837{ssp1_target_max283x};
max2839::MAX2839 second_if_max2839{ssp1_target_max283x};
#ifdef PRALINE
max2831::MAX2831 second_if_max2831{ssp1_target_max283x};
#endif
static max5864::MAX5864 baseband_codec{ssp1_target_max5864};
static baseband::CPLD baseband_cpld;

// load_sram() is called at boot in portapack.cpp, including verify CPLD part, so default direction is Receive
static rf::Direction direction{rf::Direction::Receive};
static bool baseband_invert = false;
static bool mixer_invert = false;

void init() {
#ifdef PRALINE
    /* PRALINE uses MAX2831 transceiver */
    second_if = (max283x::MAX283x*)&second_if_max2831;
#else
    if (hackrf_r9) {
        gpio_r9_not_ant_pwr.write(1);
        gpio_r9_not_ant_pwr.output();
    }
    second_if = hackrf_r9
                    ? (max283x::MAX283x*)&second_if_max2839
                    : (max283x::MAX283x*)&second_if_max2837;
#endif
    rf_path.init();
    first_if.init();
    second_if->init();
    baseband_codec.init();
#ifndef PRALINE
    /* HackRF One uses CPLD for Q inversion control.
     * PRALINE uses FPGA and the pin (P2_3) is used for LCD_TE on H4M. */
    baseband_cpld.init();
#else
    /* Initialize FPGA registers - DC_BLOCK must be enabled for RX */
    debug::fpga::init();
#endif
}

void set_direction(const rf::Direction new_direction) {
    /* TODO: Refactor all the various "Direction" enumerations into one. */
    /* TODO: Only make changes if direction changes, but beware of clock enabling. */

    // That below code line , was used to prevent RX interf ghosting when switching back to RX from any TX mode, but in recent code. it seems not necessary.
    // Deleting that load_sram_no_verify() (or the original , load_sram() ), solves random TX swap I/Q  problem in H1R1 , others OK- (and no side effects to all).
    // hackrf::cpld::load_sram_no_verify();  // After commit "removed the use of the hackrf cpld eeprom #1732", in a H1R1,  Mic App wrong SSB TX with random USB/LSB change.

    direction = new_direction;

    if (hackrf_r9) {
        /*
         * HackRF One r9 inverts analog baseband only for RX. Previous hardware
         * revisions inverted analog baseband for neither direction because of
         * compensation in the CPLD. If we ever simplify the CPLD to handle RX
         * and TX the same way, we will need to update this baseband_invert
         * logic.
         */
        baseband_invert = (direction == rf::Direction::Receive);
    } else {
        /*
         * Analog baseband is inverted in RX but not TX. The RX inversion is
         * corrected by the CPLD, but future hardware or CPLD changes may
         * change this for either or both directions. For a given hardware+CPLD
         * platform, baseband inversion is set here for RX and/or TX. Spectrum
         * inversion resulting from the mixer is tracked separately according
         * to the tuning configuration. We ask the CPLD to apply a correction
         * for the total inversion.
         */
        baseband_invert = false;
    }
#ifndef PRALINE
    baseband_cpld.set_invert(mixer_invert ^ baseband_invert);
#endif

    second_if->set_mode((direction == rf::Direction::Transmit) ? max283x::Mode::Transmit : max283x::Mode::Receive);
    rf_path.set_direction(direction);

    baseband_codec.set_mode((direction == rf::Direction::Transmit) ? max5864::Mode::Transmit : max5864::Mode::Receive);

    if (direction == rf::Direction::Receive)
        led_rx.on();
    else
        led_tx.on();
}

bool set_tuning_frequency(const rf::Frequency frequency) {
    rf::Frequency final_frequency = frequency;
    // if converter feature is enabled
    if (portapack::persistent_memory::config_converter()) {
        // downconvert
        if (portapack::persistent_memory::config_updown_converter()) {
            final_frequency = frequency - portapack::persistent_memory::config_converter_freq();
        } else  // upconvert
        {
            final_frequency = frequency + portapack::persistent_memory::config_converter_freq();
        }
    }
    // apply frequency correction
    if (direction == rf::Direction::Transmit) {
        if (portapack::persistent_memory::config_freq_tx_correction_updown())  // tx freq correction down
            final_frequency = final_frequency - portapack::persistent_memory::config_freq_tx_correction();
        else  // tx freq correction up
            final_frequency = final_frequency + portapack::persistent_memory::config_freq_tx_correction();
    } else {
        if (portapack::persistent_memory::config_freq_rx_correction_updown())  // rx freq correction down
            final_frequency = final_frequency - portapack::persistent_memory::config_freq_rx_correction();
        else  // rx freq correction up
            final_frequency = final_frequency + portapack::persistent_memory::config_freq_rx_correction();
    }

    const auto tuning_config = tuning::config::create(final_frequency);
    if (tuning_config.is_valid()) {
        first_if.disable();

        // Program first local oscillator frequency (if there is one) into RFFC507x
        if (tuning_config.first_lo_frequency) {
            first_if.set_frequency(tuning_config.first_lo_frequency);
            first_if.enable();
        }

        // Program second local oscillator frequency into MAX283x
        const auto result_second_if = second_if->set_frequency(tuning_config.second_lo_frequency);

        rf_path.set_band(tuning_config.rf_path_band);
        mixer_invert = tuning_config.mixer_invert;
#ifndef PRALINE
        baseband_cpld.set_invert(mixer_invert ^ baseband_invert);
#endif

        return result_second_if;
    } else {
        return false;
    }
}

void set_rf_amp(const bool rf_amp) {
    rf_path.set_rf_amp(rf_amp);
}

void set_lna_gain(const int_fast8_t db) {
    second_if->set_lna_gain(db);
}

void set_vga_gain(const int_fast8_t db) {
    second_if->set_vga_gain(db);
}

void set_tx_gain(const int_fast8_t db) {
    second_if->set_tx_vga_gain(db);
}

void set_baseband_filter_bandwidth_rx(const uint32_t bandwidth_minimum) {
    second_if->set_lpf_rf_bandwidth_rx(bandwidth_minimum);
}

void set_baseband_filter_bandwidth_tx(const uint32_t bandwidth_minimum) {
    second_if->set_lpf_rf_bandwidth_tx(bandwidth_minimum);
}

void set_baseband_rate(const uint32_t rate) {
    portapack::clock_manager.set_sampling_frequency(rate);
    // TODO: actually set baseband too?
}

void set_antenna_bias(const bool on) {
    /* Pull MOSFET gate low to turn on antenna bias. */
    if (hackrf_r9) {
        gpio_r9_not_ant_pwr.write(on ? 0 : 1);
    } else {
        first_if.set_gpo1(on ? 0 : 1);
    }
}

void set_tx_max283x_iq_phase_calibration(const size_t v) {
    second_if->set_tx_LO_iq_phase_calibration(v);
}

void set_rx_max283x_iq_phase_calibration(const size_t v) {
    second_if->set_rx_LO_iq_phase_calibration(v);
}

/*void enable(Configuration configuration) {
    configure(configuration);
}

void configure(Configuration configuration) {
    set_tuning_frequency(configuration.tuning_frequency);
    set_rf_amp(configuration.rf_amp);
    set_lna_gain(configuration.lna_gain);
    set_vga_gain(configuration.vga_gain);
    set_baseband_rate(configuration.baseband_rate);
    set_baseband_filter_bandwidth(configuration.baseband_filter_bandwidth);
    set_direction(configuration.direction);
}*/

void disable() {
    set_antenna_bias(false);
    baseband_codec.set_mode(max5864::Mode::Shutdown);
    second_if->set_mode(max2837::Mode::Standby);
    first_if.disable();
    set_rf_amp(false);

    led_rx.off();
    led_tx.off();
}

namespace debug {

namespace first_if {

uint32_t register_read(const size_t register_number) {
    return radio::first_if.read(register_number);
}

void register_write(const size_t register_number, uint32_t value) {
    radio::first_if.write(register_number, value);
}

} /* namespace first_if */

namespace second_if {

uint32_t register_read(const size_t register_number) {
    return radio::second_if->read(register_number);
}

void register_write(const size_t register_number, uint32_t value) {
    radio::second_if->write(register_number, value);
}

int8_t temp_sense() {
    return radio::second_if->temp_sense();
}

} /* namespace second_if */

#ifdef PRALINE
namespace fpga {

// Direct SSP1 register access - bypassing ChibiOS driver entirely
// This matches the approach used in fpga_bridge.c during early boot

// SSP1 register addresses
#define SSP1_BASE       0x400C5000
#define SSP1_CR0        (*reinterpret_cast<volatile uint32_t*>(SSP1_BASE + 0x000))
#define SSP1_CR1        (*reinterpret_cast<volatile uint32_t*>(SSP1_BASE + 0x004))
#define SSP1_DR         (*reinterpret_cast<volatile uint32_t*>(SSP1_BASE + 0x008))
#define SSP1_SR         (*reinterpret_cast<volatile uint32_t*>(SSP1_BASE + 0x00C))
#define SSP1_CPSR       (*reinterpret_cast<volatile uint32_t*>(SSP1_BASE + 0x010))

// SSP status bits
#define SSP_SR_TNF      (1 << 1)  // TX FIFO not full
#define SSP_SR_RNE      (1 << 2)  // RX FIFO not empty
#define SSP_SR_BSY      (1 << 4)  // Busy

// GPIO direct access for chip select (GPIO2[10])
#define GPIO_PORT2_SET  (*reinterpret_cast<volatile uint32_t*>(0x400F4000 + 0x2200 + 2*4))
#define GPIO_PORT2_CLR  (*reinterpret_cast<volatile uint32_t*>(0x400F4000 + 0x2280 + 2*4))
#define GPIO_PORT2_DIR  (*reinterpret_cast<volatile uint32_t*>(0x400F4000 + 0x2000 + 2*4))
#define FPGA_CS_BIT     (1 << 10)

static void fpga_cs_high() {
    GPIO_PORT2_SET = FPGA_CS_BIT;
}

static void fpga_cs_low() {
    GPIO_PORT2_CLR = FPGA_CS_BIT;
}

static void fpga_cs_output() {
    GPIO_PORT2_DIR |= FPGA_CS_BIT;
}

// Transfer one byte via SSP1, return received byte
static uint8_t ssp1_transfer_byte(uint8_t data) {
    // Wait for TX FIFO not full
    while ((SSP1_SR & SSP_SR_TNF) == 0) {}
    SSP1_DR = data;
    // Wait for not busy
    while (SSP1_SR & SSP_SR_BSY) {}
    // Wait for RX FIFO not empty
    while ((SSP1_SR & SSP_SR_RNE) == 0) {}
    return SSP1_DR;
}

// Configure SSP1 for iCE40 FPGA (Mode 3: CPOL=1, CPHA=1, 8-bit)
static void ssp1_config_fpga_mode() {
    SSP1_CR1 = 0;  // Disable SSP1
    // Mode 3: CPOL=1 (bit 6), CPHA=1 (bit 7), 8-bit (DSS=7), SCR=21
    SSP1_CR0 = 7 | (1 << 6) | (1 << 7) | (21 << 8);
    SSP1_CPSR = 2;  // Clock prescaler
    SSP1_CR1 = (1 << 1);  // Enable SSP1
}

// Configure SSP1 back for MAX2831 (Mode 0: CPOL=0, CPHA=0, 9-bit)
static void ssp1_config_max2831_mode() {
    SSP1_CR1 = 0;  // Disable SSP1
    // Mode 0: CPOL=0, CPHA=0, 9-bit (DSS=8), SCR=24
    SSP1_CR0 = 8 | (24 << 8);
    SSP1_CPSR = 2;
    SSP1_CR1 = (1 << 1);  // Enable SSP1
}

uint32_t register_read(const size_t register_number) {
    if (register_number == 0 || register_number > 5) return 0xFF;

    // Ensure CS is output and deselected
    fpga_cs_output();
    fpga_cs_high();

    // Switch to FPGA SPI mode
    ssp1_config_fpga_mode();

    // CS low
    fpga_cs_low();

    // SPI protocol: [reg & 0x7F, 0x00, 0x00] -> value in byte 3
    ssp1_transfer_byte(register_number & 0x7F);
    ssp1_transfer_byte(0x00);
    uint8_t value = ssp1_transfer_byte(0x00);

    // CS high
    fpga_cs_high();

    // Switch back to MAX2831 mode
    ssp1_config_max2831_mode();

    return value;
}

void register_write(const size_t register_number, uint32_t value) {
    if (register_number == 0 || register_number > 5) return;

    // Ensure CS is output and deselected
    fpga_cs_output();
    fpga_cs_high();

    // Switch to FPGA SPI mode
    ssp1_config_fpga_mode();

    // CS low
    fpga_cs_low();

    // SPI protocol: [(reg | 0x80), value, 0x00]
    ssp1_transfer_byte((register_number & 0x7F) | 0x80);
    ssp1_transfer_byte(value);
    ssp1_transfer_byte(0x00);

    // CS high
    fpga_cs_high();

    // Switch back to MAX2831 mode
    ssp1_config_max2831_mode();
}

void init() {
    // Configure FPGA CS GPIO as output, initially deselected
    fpga_cs_output();
    fpga_cs_high();

    // Initialize FPGA registers after bitstream load
    // DC_BLOCK (bit 0) must be enabled for RX to work
    register_write(1, 0x01);  // CTRL: DC_BLOCK=1
    register_write(2, 0x00);  // RX_DECIM: no decimation
    register_write(3, 0x00);  // TX_CTRL: NCO disabled
    register_write(4, 0x00);  // TX_INTRP: no interpolation
    register_write(5, 0x00);  // TX_PSTEP: zero phase step
}

} /* namespace fpga */
#endif

} /* namespace debug */

} /* namespace radio */
