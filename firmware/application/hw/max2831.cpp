/*
 * Copyright (C) 2025 Great Scott Gadgets
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

#include "max2831.hpp"

#include "hackrf_hal.hpp"
#include "hackrf_gpio.hpp"
using namespace hackrf::one;

#include "ch.h"
#include "hal.h"

#include <algorithm>

namespace max2831 {

using namespace max283x;

/*
 * MAX2831 uses 9-bit SPI transfers.
 * An 18-bit word is sent as two 9-bit transfers:
 *   Word format: [VALUE:14][REG:4]
 *   First transfer: bits 17:9 (high 9 bits)
 *   Second transfer: bits 8:0 (low 9 bits)
 */
void MAX2831::write_reg(const uint8_t reg, const uint16_t value) {
    uint32_t word = (((uint32_t)value & 0x3fff) << 4) | (reg & 0xf);
    uint16_t values[2] = {
        static_cast<uint16_t>(word >> 9),
        static_cast<uint16_t>(word & 0x1ff)
    };
    _target.transfer(values, 2);
}

void MAX2831::flush_reg(const uint8_t reg) {
    write_reg(reg, _regs[reg]);
}

void MAX2831::flush_all() {
    for (size_t r = 0; r < reg_count; r++) {
        flush_reg(r);
    }
}

void MAX2831::init() {
    set_mode(Mode::Shutdown);

    /* Configure GPIO pins for MAX2831 control */
    gpio_max283x_enable.output();
    gpio_max2831_rx_enable.output();
    gpio_max2831_rxhp.output();
    gpio_max2831_rxhp.write(0);  /* RXHP low = 100 Hz HPF (default) */

    /* Reset to default register values */
    _regs = default_regs;

    /* Enable SPI control of gain settings */
    _regs[8] |= REG8_RXVGA_GAIN_SPI_EN;  /* RX VGA gain via SPI */
    _regs[9] |= REG9_TXVGA_GAIN_SPI_EN;  /* TX VGA gain via SPI */

    /* Set initial gains */
    _regs[11] = (_regs[11] & ~REG11_LNA_GAIN_MASK) | REG11_LNA_GAIN_MAX;
    _regs[11] = (_regs[11] & ~REG11_RXVGA_GAIN_MASK) | 0x18;  /* Moderate RX VGA gain */
    _regs[12] = (_regs[12] & ~REG12_TXVGA_GAIN_MASK) | 0x00;  /* Minimum TX gain */

    /* Configure LPF for reasonable bandwidth */
    _regs[8] = (_regs[8] & ~REG8_RX_LPF_COARSE_MASK) | REG8_RX_LPF_7_5M;
    _regs[8] = (_regs[8] & ~REG8_RX_LPF_FINE_MASK) | REG8_RX_LPF_FINE_100;
    _regs[7] = (_regs[7] & ~REG7_TX_LPF_COARSE_MASK) | REG7_TX_LPF_8M;
    _regs[7] = (_regs[7] & ~REG7_TX_LPF_FINE_MASK) | REG7_TX_LPF_FINE_100;

    /* Disable clock output */
    _regs[14] &= ~REG14_CLKOUT_EN;

    /* Write all registers */
    flush_all();

    set_mode(Mode::Standby);
}

void MAX2831::set_mode(const Mode mode) {
    _mode = mode;

    /*
     * MAX2831 mode control via ENABLE and RXTX pins.
     * From hackrf max2831_target.c:
     *
     *   Shutdown: ENABLE=0, RXTX=0
     *   Standby:  ENABLE=0, RXTX=1  (PLL/VCO/LO on, ready for quick TX/RX)
     *   RX:       ENABLE=1, RXTX=0
     *   TX:       ENABLE=1, RXTX=1
     *
     * Note: gpio_max2831_rx_enable is actually the RXTX mode select pin.
     * RXTX=0 selects RX, RXTX=1 selects TX.
     */
    switch (mode) {
        default:
        case Mode::Shutdown:
            gpio_max2831_rx_enable.write(0);  /* RXTX=0 */
            gpio_max283x_enable.write(0);     /* ENABLE=0 */
            break;
        case Mode::Standby:
            gpio_max2831_rx_enable.write(1);  /* RXTX=1 */
            gpio_max283x_enable.write(0);     /* ENABLE=0 */
            break;
        case Mode::Transmit:
        case Mode::Tx_Calibration:
            gpio_max2831_rx_enable.write(1);  /* RXTX=1 for TX */
            gpio_max283x_enable.write(1);     /* ENABLE=1 */
            break;
        case Mode::Receive:
        case Mode::Rx_Calibration:
            gpio_max2831_rx_enable.write(0);  /* RXTX=0 for RX */
            gpio_max283x_enable.write(1);     /* ENABLE=1 */
            break;
    }
}

void MAX2831::set_tx_vga_gain(const int_fast8_t db) {
    /* TX VGA gain: 0-31 dB in ~1 dB steps */
    int_fast8_t db_clipped = std::max(0, std::min(31, (int)db));
    /* Register value: gain * 2 | 1, max 0x3F */
    uint16_t value = std::min((db_clipped << 1) | 1, 0x3f);
    _regs[12] = (_regs[12] & ~REG12_TXVGA_GAIN_MASK) | value;
    flush_reg(12);
}

void MAX2831::set_lna_gain(const int_fast8_t db) {
    /*
     * LNA gain has 3 settings:
     *   MAX (33 dB), -16 dB from max (17 dB), -33 dB from max (0 dB)
     * Map from MAX2837 8 dB steps for compatibility
     */
    uint16_t gain_val;
    if (db >= 32) {
        gain_val = REG11_LNA_GAIN_MAX;
    } else if (db >= 16) {
        gain_val = REG11_LNA_GAIN_M16;
    } else {
        gain_val = REG11_LNA_GAIN_M33;
    }
    _regs[11] = (_regs[11] & ~REG11_LNA_GAIN_MASK) | gain_val;
    flush_reg(11);
}

void MAX2831::set_vga_gain(const int_fast8_t db) {
    /* VGA gain: 0-62 dB in 2 dB steps */
    int_fast8_t db_clipped = std::max(0, std::min(62, (int)db));
    uint16_t value = (db_clipped >> 1) & 0x1f;
    _regs[11] = (_regs[11] & ~REG11_RXVGA_GAIN_MASK) | value;
    flush_reg(11);
}

void MAX2831::set_lpf_rf_bandwidth_rx(const uint32_t bandwidth_minimum) {
    /* RX LPF bandwidths (approximate -0.5 dB points):
     *   7.5 MHz, 8.5 MHz, 15 MHz, 18 MHz
     * With fine adjustment: 90%, 95%, 100%, 105%, 110%
     */
    uint16_t coarse;
    if (bandwidth_minimum <= 11600000) {
        coarse = REG8_RX_LPF_7_5M;
    } else if (bandwidth_minimum <= 15100000) {
        coarse = REG8_RX_LPF_8_5M;
    } else if (bandwidth_minimum <= 22600000) {
        coarse = REG8_RX_LPF_15M;
    } else {
        coarse = REG8_RX_LPF_18M;
    }

    _regs[8] = (_regs[8] & ~REG8_RX_LPF_COARSE_MASK) | coarse;
    _regs[8] = (_regs[8] & ~REG8_RX_LPF_FINE_MASK) | REG8_RX_LPF_FINE_100;
    flush_reg(8);
}

void MAX2831::set_lpf_rf_bandwidth_tx(const uint32_t bandwidth_minimum) {
    /* TX LPF bandwidths (approximate -0.5 dB points):
     *   8 MHz, 11 MHz, 16.5 MHz, 22.5 MHz
     * With fine adjustment: 90%, 95%, 100%, 105%, 110%, 115%
     */
    uint16_t coarse;
    if (bandwidth_minimum <= 11900000) {
        coarse = REG7_TX_LPF_8M;
    } else if (bandwidth_minimum <= 15800000) {
        coarse = REG7_TX_LPF_11M;
    } else if (bandwidth_minimum <= 23600000) {
        coarse = REG7_TX_LPF_16_5M;
    } else {
        coarse = REG7_TX_LPF_22_5M;
    }

    _regs[7] = (_regs[7] & ~REG7_TX_LPF_COARSE_MASK) | coarse;
    _regs[7] = (_regs[7] & ~REG7_TX_LPF_FINE_MASK) | REG7_TX_LPF_FINE_100;
    flush_reg(7);
}

bool MAX2831::set_frequency(const rf::Frequency lo_frequency) {
    /*
     * MAX2831 frequency synthesis:
     *   F_LO = F_REF * (N + F/2^20) / R
     * Where:
     *   F_REF = 40 MHz reference
     *   R = reference divider (1 or 2)
     *   N = integer divider (8 bits)
     *   F = fractional divider (20 bits)
     *
     * Using R=2: F_LO = 40M * (N + F/2^20) / 2 = 20M * (N + F/2^20)
     */

    /* MAX2831 supports 2.3-2.6 GHz */
    if (lo_frequency < 2300000000 || lo_frequency > 2600000000) {
        return false;
    }

    constexpr uint32_t ref_freq = 20000000;  /* 40 MHz / 2 */

    /* Calculate integer and fractional parts */
    uint64_t ratio_q20 = (static_cast<uint64_t>(lo_frequency) << 20) / ref_freq;
    uint16_t n = ratio_q20 >> 20;
    uint32_t f = ratio_q20 & 0xFFFFF;

    /* Split fractional into low 6 bits and high 14 bits */
    uint16_t frac_lo = f & 0x3F;
    uint16_t frac_hi = (f >> 6) & 0x3FFF;

    /* Update registers */
    _regs[0] = (_regs[0] & ~REG0_SYN_FRAC_LO_MASK) | frac_lo;
    _regs[0] = (_regs[0] & ~REG0_SYN_INT_MASK) | ((n << REG0_SYN_INT_SHIFT) & REG0_SYN_INT_MASK);
    _regs[3] = frac_hi;

    /* Write in correct order */
    flush_reg(0);
    flush_reg(3);

    return true;
}

void MAX2831::set_rx_LO_iq_phase_calibration(const size_t v) {
    /* MAX2831 doesn't have the same IQ calibration as MAX2837 */
    (void)v;
}

void MAX2831::set_tx_LO_iq_phase_calibration(const size_t v) {
    /* MAX2831 doesn't have the same IQ calibration as MAX2837 */
    (void)v;
}

void MAX2831::set_rx_buff_vcm(const size_t v) {
    /* MAX2831 RX IQ common mode voltage is in register 15 */
    /* Values: 0=0.9V, 1=1.0V, 2=1.1V, 3=1.2V */
    uint16_t vcm = std::min(v, (size_t)3);
    _regs[15] = (_regs[15] & ~0x0003) | vcm;
    flush_reg(15);
}

int8_t MAX2831::temp_sense() {
    /* MAX2831 temperature sensor can be read via RSSI MUX
     * This is a simplified implementation - returns a fixed value
     * A full implementation would switch RSSI_MUX to temperature,
     * read the ADC, then switch back.
     */
    return 25;  /* Room temperature placeholder */
}

reg_t MAX2831::read(const address_t reg_num) {
    /* MAX2831 doesn't support SPI read, return cached value */
    if (reg_num < reg_count) {
        return _regs[reg_num];
    }
    return 0;
}

void MAX2831::write(const address_t reg_num, const reg_t value) {
    if (reg_num < reg_count) {
        _regs[reg_num] = value & 0x3FFF;  /* 14-bit registers */
        write_reg(reg_num, _regs[reg_num]);
    }
}

}  // namespace max2831
