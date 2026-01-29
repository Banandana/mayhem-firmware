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

#ifndef __MAX2831_H__
#define __MAX2831_H__

#include "max283x.hpp"
#include "gpio.hpp"
#include "spi_arbiter.hpp"

#include <cstdint>
#include <array>

namespace max2831 {

using namespace max283x;

/* MAX2831 has 16 registers, each containing 14 bits of data */
constexpr size_t reg_count = 16;

/* Default register values from MAX2831 datasheet and hackrf driver */
constexpr std::array<uint16_t, reg_count> default_regs = {
    0x1740, /* 0: enable fractional mode */
    0x119a, /* 1 */
    0x1003, /* 2 */
    0x0079, /* 3: PLL divider settings for 2437 MHz */
    0x3666, /* 4: PLL divider settings for 2437 MHz */
    0x00a4, /* 5: divide reference frequency by 2 */
    0x0060, /* 6: enable TX power detector */
    0x1022, /* 7: 110% TX LPF bandwidth */
    0x2021, /* 8: pin control of RX gain, 11 MHz LPF bandwidth */
    0x03b5, /* 9: pin control of TX gain */
    0x1d80, /* 10: 3.5 us PA enable delay, zero PA bias */
    0x0074, /* 11: LNA high gain, RX VGA moderate gain */
    0x0140, /* 12: TX VGA minimum */
    0x0e92, /* 13 */
    0x0100, /* 14: reference clock output disabled */
    0x0145, /* 15: RX IQ common mode 1.1 V */
};

/* Register bit field definitions */

/* Register 0: Synthesizer Integer Divider */
constexpr uint16_t REG0_SYN_FRAC_LO_MASK = 0x003F;  /* D5:D0 - Low 6 bits of fractional divider */
constexpr uint16_t REG0_SYN_INT_MASK = 0x3FC0;      /* D13:D6 - Integer divider */
constexpr uint16_t REG0_SYN_INT_SHIFT = 6;

/* Register 3: Synthesizer Fractional Divider High */
constexpr uint16_t REG3_SYN_FRAC_HI_MASK = 0x3FFF;  /* D13:D0 - High 14 bits of fractional divider */

/* Register 5: Reference Divider */
constexpr uint16_t REG5_REF_DIV_1 = 0x00;
constexpr uint16_t REG5_REF_DIV_2 = 0x04;  /* Divide by 2 */

/* Register 7: TX LPF */
constexpr uint16_t REG7_TX_LPF_FINE_90 = 0x00;
constexpr uint16_t REG7_TX_LPF_FINE_95 = 0x01;
constexpr uint16_t REG7_TX_LPF_FINE_100 = 0x02;
constexpr uint16_t REG7_TX_LPF_FINE_105 = 0x03;
constexpr uint16_t REG7_TX_LPF_FINE_110 = 0x04;
constexpr uint16_t REG7_TX_LPF_FINE_115 = 0x05;
constexpr uint16_t REG7_TX_LPF_FINE_MASK = 0x0007;

constexpr uint16_t REG7_TX_LPF_8M = 0x00;
constexpr uint16_t REG7_TX_LPF_11M = 0x08;
constexpr uint16_t REG7_TX_LPF_16_5M = 0x10;
constexpr uint16_t REG7_TX_LPF_22_5M = 0x18;
constexpr uint16_t REG7_TX_LPF_COARSE_MASK = 0x0018;
constexpr uint16_t REG7_TX_LPF_COARSE_SHIFT = 3;

constexpr uint16_t REG7_RX_HPF_100HZ = 0x00;
constexpr uint16_t REG7_RX_HPF_4KHZ = 0x1000;
constexpr uint16_t REG7_RX_HPF_30KHZ = 0x2000;
constexpr uint16_t REG7_RX_HPF_MASK = 0x3000;

/* Register 8: RX LPF */
constexpr uint16_t REG8_RX_LPF_FINE_90 = 0x00;
constexpr uint16_t REG8_RX_LPF_FINE_95 = 0x01;
constexpr uint16_t REG8_RX_LPF_FINE_100 = 0x02;
constexpr uint16_t REG8_RX_LPF_FINE_105 = 0x03;
constexpr uint16_t REG8_RX_LPF_FINE_110 = 0x04;
constexpr uint16_t REG8_RX_LPF_FINE_MASK = 0x0007;

constexpr uint16_t REG8_RX_LPF_7_5M = 0x00;
constexpr uint16_t REG8_RX_LPF_8_5M = 0x08;
constexpr uint16_t REG8_RX_LPF_15M = 0x10;
constexpr uint16_t REG8_RX_LPF_18M = 0x18;
constexpr uint16_t REG8_RX_LPF_COARSE_MASK = 0x0018;
constexpr uint16_t REG8_RX_LPF_COARSE_SHIFT = 3;

constexpr uint16_t REG8_RXVGA_GAIN_SPI_EN = 0x2000;

/* Register 9: TX Gain Control */
constexpr uint16_t REG9_TXVGA_GAIN_SPI_EN = 0x0200;

/* Register 11: RX Gain */
constexpr uint16_t REG11_RXVGA_GAIN_MASK = 0x001F;
constexpr uint16_t REG11_LNA_GAIN_MAX = 0x0060;    /* Maximum LNA gain */
constexpr uint16_t REG11_LNA_GAIN_M16 = 0x0040;   /* -16 dB from max */
constexpr uint16_t REG11_LNA_GAIN_M33 = 0x0000;   /* -33 dB from max (min) */
constexpr uint16_t REG11_LNA_GAIN_MASK = 0x0060;

/* Register 12: TX VGA Gain */
constexpr uint16_t REG12_TXVGA_GAIN_MASK = 0x003F;

/* Register 14: Clock Output */
constexpr uint16_t REG14_CLKOUT_EN = 0x0020;

class MAX2831 : public MAX283x {
   public:
    constexpr MAX2831(
        spi::arbiter::Target& target)
        : _target(target) {
    }

    void init() override;
    void set_mode(const Mode mode) override;

    void set_tx_vga_gain(const int_fast8_t db) override;
    void set_lna_gain(const int_fast8_t db) override;
    void set_vga_gain(const int_fast8_t db) override;
    void set_lpf_rf_bandwidth_rx(const uint32_t bandwidth_minimum) override;
    void set_lpf_rf_bandwidth_tx(const uint32_t bandwidth_minimum) override;

    bool set_frequency(const rf::Frequency lo_frequency) override;

    void set_rx_LO_iq_phase_calibration(const size_t v) override;
    void set_tx_LO_iq_phase_calibration(const size_t v) override;

    void set_rx_buff_vcm(const size_t v) override;

    int8_t temp_sense() override;

    reg_t read(const address_t reg_num) override;
    void write(const address_t reg_num, const reg_t value) override;

   private:
    spi::arbiter::Target& _target;
    Mode _mode{Mode::Standby};
    std::array<uint16_t, reg_count> _regs{default_regs};

    void write_reg(const uint8_t reg, const uint16_t value);
    void flush_reg(const uint8_t reg);
    void flush_all();
};

}  // namespace max2831

#endif /*__MAX2831_H__*/
