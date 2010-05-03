//
// Copyright 2010 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

// TX IO Pins
#define HB_PA_OFF_TXIO      (1 << 15)    // 5GHz PA, 1 = off, 0 = on
#define LB_PA_OFF_TXIO      (1 << 14)    // 2.4GHz PA, 1 = off, 0 = on
#define ANTSEL_TX1_RX2_TXIO (1 << 13)    // 1 = Ant 1 to TX, Ant 2 to RX
#define ANTSEL_TX2_RX1_TXIO (1 << 12)    // 1 = Ant 2 to TX, Ant 1 to RX
#define TX_EN_TXIO          (1 << 11)    // 1 = TX on, 0 = TX off
#define AD9515DIV_TXIO      (1 << 4)     // 1 = Div  by 3, 0 = Div by 2

#define TXIO_MASK (HB_PA_OFF_TXIO | LB_PA_OFF_TXIO | ANTSEL_TX1_RX2_TXIO | ANTSEL_TX2_RX1_TXIO | TX_EN_TXIO | AD9515DIV_TXIO)

// TX IO Functions
#define HB_PA_TXIO               LB_PA_OFF_TXIO
#define LB_PA_TXIO               HB_PA_OFF_TXIO
#define TX_ENB_TXIO              TX_EN_TXIO
#define TX_DIS_TXIO              0
#define AD9515DIV_3_TXIO         AD9515DIV_TXIO
#define AD9515DIV_2_TXIO         0

// RX IO Pins
#define LOCKDET_RXIO (1 << 15)           // This is an INPUT!!!
#define POWER_RXIO   (1 << 14)           // 1 = power on, 0 = shutdown
#define RX_EN_RXIO   (1 << 13)           // 1 = RX on, 0 = RX off
#define RX_HP_RXIO   (1 << 12)           // 0 = Fc set by rx_hpf, 1 = 600 KHz

#define RXIO_MASK (POWER_RXIO | RX_EN_RXIO | RX_HP_RXIO)

// RX IO Functions
#define POWER_UP_RXIO            POWER_RXIO
#define POWER_DOWN_RXIO          0
#define RX_ENB_RXIO              RX_EN_RXIO
#define RX_DIS_RXIO              0

#include "max2829_regs.hpp"
#include <uhd/utils/static.hpp>
#include <uhd/utils/assert.hpp>
#include <uhd/utils/algorithm.hpp>
#include <uhd/types/ranges.hpp>
#include <uhd/types/dict.hpp>
#include <uhd/usrp/subdev_props.hpp>
#include <uhd/usrp/dboard_base.hpp>
#include <uhd/usrp/dboard_manager.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/math/special_functions/round.hpp>
#include <utility>

using namespace uhd;
using namespace uhd::usrp;
using namespace boost::assign;

/***********************************************************************
 * The XCVR 2450 constants
 **********************************************************************/
static const bool xcvr2450_debug = false;

static const freq_range_t xcvr_freq_range(2.4e9, 6.0e9);

static const prop_names_t xcvr_antennas = list_of("J1")("J2");

static const uhd::dict<std::string, gain_range_t> xcvr_tx_gain_ranges = map_list_of
    ("VGA", gain_range_t(0, 30, 0.5))
    ("BB", gain_range_t(0, 5, 1.5))
;
static const uhd::dict<std::string, gain_range_t> xcvr_rx_gain_ranges = map_list_of
    ("LNA", gain_range_t(0, 30.5, 15))
    ("VGA", gain_range_t(0, 62, 2.0))
;

/***********************************************************************
 * The XCVR 2450 dboard class
 **********************************************************************/
class xcvr2450 : public xcvr_dboard_base{
public:
    xcvr2450(ctor_args_t args);
    ~xcvr2450(void);

    void rx_get(const wax::obj &key, wax::obj &val);
    void rx_set(const wax::obj &key, const wax::obj &val);

    void tx_get(const wax::obj &key, wax::obj &val);
    void tx_set(const wax::obj &key, const wax::obj &val);

private:
    double _lo_freq;
    uhd::dict<std::string, float> _tx_gains, _rx_gains;
    std::string _tx_ant, _rx_ant;
    int _ad9515div;
    max2829_regs_t _max2829_regs;

    void set_lo_freq(double target_freq);
    void set_tx_ant(const std::string &ant);
    void set_rx_ant(const std::string &ant);
    void set_tx_gain(float gain, const std::string &name);
    void set_rx_gain(float gain, const std::string &name);

    void update_atr(void);
    void spi_reset(void);
    void send_reg(boost::uint8_t addr){
        boost::uint32_t value = _max2829_regs.get_reg(addr);
        if(xcvr2450_debug) std::cerr << boost::format(
            "XCVR2450: send reg 0x%02x, value 0x%05x"
        ) % int(addr) % value << std::endl;
        this->get_iface()->write_spi(
            dboard_iface::UNIT_RX,
            spi_config_t::EDGE_RISE,
            value, 24
        );
    }

    static bool is_highband(double freq){return freq > 3e9;}

    /*!
     * Is the LO locked?
     * \return true for locked
     */
    bool get_locked(void){
        return (this->get_iface()->read_gpio(dboard_iface::UNIT_RX) & LOCKDET_RXIO) != 0;
    }

    /*!
     * Read the RSSI from the aux adc
     * \return the rssi in dB
     */
    float get_rssi(void){
        //constants for the rssi calculation
        static const float min_v = float(0.5), max_v = float(2.5);
        static const float rssi_dyn_range = 60;
        //calculate the rssi from the voltage
        float voltage = this->get_iface()->read_aux_adc(dboard_iface::UNIT_RX, 1);
        return rssi_dyn_range*(voltage - min_v)/(max_v - min_v);
    }
};

/***********************************************************************
 * Register the XCVR 2450 dboard
 **********************************************************************/
static dboard_base::sptr make_xcvr2450(dboard_base::ctor_args_t args){
    return dboard_base::sptr(new xcvr2450(args));
}

UHD_STATIC_BLOCK(reg_xcvr2450_dboard){
    //register the factory function for the rx and tx dbids
    dboard_manager::register_dboard(0x0060, &make_xcvr2450, "XCVR2450 TX");
    dboard_manager::register_dboard(0x0061, &make_xcvr2450, "XCVR2450 RX");
}

/***********************************************************************
 * Structors
 **********************************************************************/
xcvr2450::xcvr2450(ctor_args_t args) : xcvr_dboard_base(args){
    //enable only the clocks we need
    this->get_iface()->set_clock_enabled(dboard_iface::UNIT_TX, true);

    //set the gpio directions
    this->get_iface()->set_gpio_ddr(dboard_iface::UNIT_TX, TXIO_MASK);
    this->get_iface()->set_gpio_ddr(dboard_iface::UNIT_RX, RXIO_MASK);

    spi_reset(); //prepare the spi

    //setup the misc max2829 registers
    _max2829_regs.mimo_select         = max2829_regs_t::MIMO_SELECT_MIMO;
    _max2829_regs.band_sel_mimo       = max2829_regs_t::BAND_SEL_MIMO_MIMO;
    _max2829_regs.pll_cp_select       = max2829_regs_t::PLL_CP_SELECT_4MA;
    _max2829_regs.rssi_high_bw        = max2829_regs_t::RSSI_HIGH_BW_6MHZ;
    _max2829_regs.tx_lpf_coarse_adj   = max2829_regs_t::TX_LPF_COARSE_ADJ_12MHZ;
    _max2829_regs.rx_lpf_coarse_adj   = max2829_regs_t::RX_LPF_COARSE_ADJ_9_5MHZ;
    _max2829_regs.rx_lpf_fine_adj     = max2829_regs_t::RX_LPF_FINE_ADJ_95;
    _max2829_regs.rx_vga_gain_spi     = max2829_regs_t::RX_VGA_GAIN_SPI_SPI;
    _max2829_regs.rssi_output_range   = max2829_regs_t::RSSI_OUTPUT_RANGE_HIGH;
    _max2829_regs.rssi_op_mode        = max2829_regs_t::RSSI_OP_MODE_ENABLED;
    _max2829_regs.rssi_pin_fcn        = max2829_regs_t::RSSI_PIN_FCN_RSSI;
    _max2829_regs.rx_highpass         = max2829_regs_t::RX_HIGHPASS_100HZ;
    _max2829_regs.tx_vga_gain_spi     = max2829_regs_t::TX_VGA_GAIN_SPI_SPI;
    _max2829_regs.pa_driver_linearity = max2829_regs_t::PA_DRIVER_LINEARITY_78;
    _max2829_regs.tx_vga_linearity    = max2829_regs_t::TX_VGA_LINEARITY_78;
    _max2829_regs.tx_upconv_linearity = max2829_regs_t::TX_UPCONV_LINEARITY_78;

    //send initial register settings
    for(boost::uint8_t reg = 0x2; reg <= 0xC; reg++){
        this->send_reg(reg);
    }

    //set defaults for LO, gains, antennas
    set_lo_freq(2.45e9);
    set_rx_ant(xcvr_antennas.at(0));
    set_tx_ant(xcvr_antennas.at(1));
    BOOST_FOREACH(const std::string &name, xcvr_tx_gain_ranges.keys()){
        set_tx_gain(xcvr_tx_gain_ranges[name].min, name);
    }
    BOOST_FOREACH(const std::string &name, xcvr_rx_gain_ranges.keys()){
        set_rx_gain(xcvr_rx_gain_ranges[name].min, name);
    }
}

xcvr2450::~xcvr2450(void){
    spi_reset();
}

void xcvr2450::spi_reset(void){
    //spi reset mode: global enable = off, tx and rx enable = on
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_IDLE, TX_ENB_TXIO);
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_IDLE, RX_ENB_RXIO | POWER_DOWN_RXIO);
    boost::this_thread::sleep(boost::posix_time::milliseconds(10));

    //take it back out of spi reset mode and wait a bit
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_IDLE, RX_DIS_RXIO | POWER_UP_RXIO);
    boost::this_thread::sleep(boost::posix_time::milliseconds(10));
}

void xcvr2450::update_atr(void){
    //calculate tx atr pins
    int band_sel   = (xcvr2450::is_highband(_lo_freq))? HB_PA_TXIO : LB_PA_TXIO;
    int tx_ant_sel = (_tx_ant == "J1")? ANTSEL_TX1_RX2_TXIO : ANTSEL_TX2_RX1_TXIO;
    int rx_ant_sel = (_rx_ant == "J2")? ANTSEL_TX1_RX2_TXIO : ANTSEL_TX2_RX1_TXIO;
    int xx_ant_sel = tx_ant_sel; //prefer the tx antenna selection for full duplex (rx will get the other antenna)
    int ad9515div  = (_ad9515div == 3)? AD9515DIV_3_TXIO : AD9515DIV_2_TXIO;

    //set the tx registers
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_IDLE,        band_sel | ad9515div | TX_DIS_TXIO);
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_RX_ONLY,     band_sel | ad9515div | TX_DIS_TXIO | rx_ant_sel);
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_TX_ONLY,     band_sel | ad9515div | TX_ENB_TXIO | tx_ant_sel);
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_TX, dboard_iface::ATR_REG_FULL_DUPLEX, band_sel | ad9515div | TX_ENB_TXIO | xx_ant_sel);

    //set the rx registers
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_IDLE,        POWER_UP_RXIO | RX_DIS_RXIO);
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_RX_ONLY,     POWER_UP_RXIO | RX_ENB_RXIO);
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_TX_ONLY,     POWER_UP_RXIO | RX_DIS_RXIO);
    this->get_iface()->set_atr_reg(dboard_iface::UNIT_RX, dboard_iface::ATR_REG_FULL_DUPLEX, POWER_UP_RXIO | RX_ENB_RXIO);
}

/***********************************************************************
 * Tuning
 **********************************************************************/
void xcvr2450::set_lo_freq(double target_freq){
    target_freq = std::clip(target_freq, xcvr_freq_range.min, xcvr_freq_range.max);
    //TODO: clip for highband and lowband

    //variables used in the calculation below
    double scaler = xcvr2450::is_highband(target_freq)? (4.0/5.0) : (4.0/3.0);
    double ref_freq = this->get_iface()->get_clock_rate(dboard_iface::UNIT_TX);
    int R, intdiv, fracdiv;

    //loop through values until we get a match
    for(_ad9515div = 2; _ad9515div <= 3; _ad9515div++){
        for(R = 1; R <= 7; R++){
            double N = (target_freq*scaler*R*_ad9515div)/ref_freq;
            intdiv = int(std::floor(N));
            fracdiv = boost::math::iround((N - intdiv)*double(1 << 16));
            //actual minimum is 128, but most chips seems to require higher to lock
            if (intdiv < 131 or intdiv > 255) continue;
            //constraints met: exit loop
            goto done_loop;
        }
    } done_loop:

    //calculate the actual freq from the values above
    double N = double(intdiv) + double(fracdiv)/double(1 << 16);
    _lo_freq = (N*ref_freq)/(scaler*R*_ad9515div);

    if (xcvr2450_debug) std::cerr
        << boost::format("XCVR2450 tune:\n")
        << boost::format("    R=%d, N=%f, ad9515=%d, scaler=%f\n") % R % N % _ad9515div % scaler
        << boost::format("    Ref    Freq=%fMHz\n") % (ref_freq/1e6)
        << boost::format("    Target Freq=%fMHz\n") % (target_freq/1e6)
        << boost::format("    Actual Freq=%fMHz\n") % (_lo_freq/1e6)
        << std::endl;

    //high-high band or low-high band?
    if(_lo_freq > (5.35e9 + 5.47e9)/2.0){
        if (xcvr2450_debug) std::cerr << "XCVR2450 tune: Using  high-high band" << std::endl;
        _max2829_regs.band_select_802_11a = max2829_regs_t::BAND_SELECT_802_11A_5_47GHZ_TO_5_875GHZ;
    }else{
        if (xcvr2450_debug) std::cerr << "XCVR2450 tune: Using  low-high band" << std::endl;
        _max2829_regs.band_select_802_11a = max2829_regs_t::BAND_SELECT_802_11A_4_9GHZ_TO_5_35GHZ;
    }

    //new band select settings and ad9515 divider
    this->update_atr();

    //load new counters into registers
    _max2829_regs.int_div_ratio_word = intdiv;
    _max2829_regs.frac_div_ratio_lsb = fracdiv & 0x3;
    _max2829_regs.frac_div_ratio_msb = fracdiv >> 2;
    this->send_reg(0x3); //integer
    this->send_reg(0x4); //fractional

    //load the reference divider and band select into registers
    //toggle the bandswitch from off to automatic (which really means start)
    _max2829_regs.ref_divider = R;
    _max2829_regs.band_select = (xcvr2450::is_highband(_lo_freq))?
                                max2829_regs_t::BAND_SELECT_5GHZ   :
                                max2829_regs_t::BAND_SELECT_2_4GHZ ;
    _max2829_regs.vco_bandswitch = max2829_regs_t::VCO_BANDSWITCH_DISABLE;
    this->send_reg(0x5);
    _max2829_regs.vco_bandswitch = max2829_regs_t::VCO_BANDSWITCH_AUTOMATIC;;
    this->send_reg(0x5);
}

/***********************************************************************
 * Antenna Handling
 **********************************************************************/
void xcvr2450::set_tx_ant(const std::string &ant){
    assert_has(xcvr_antennas, ant, "xcvr antenna name");
   _tx_ant = ant;
    this->update_atr(); //sets the atr to the new antenna setting
}

void xcvr2450::set_rx_ant(const std::string &ant){
    assert_has(xcvr_antennas, ant, "xcvr antenna name");
    _rx_ant = ant;
    this->update_atr(); //sets the atr to the new antenna setting
}

/***********************************************************************
 * Gain Handling
 **********************************************************************/
/*!
 * Convert a requested gain for the tx vga into the integer register value.
 * The gain passed into the function will be set to the actual value.
 * \param gain the requested gain in dB
 * \return 6 bit the register value
 */
static int gain_to_tx_vga_reg(float &gain){
    //calculate the register value
    int reg = std::clip(boost::math::iround(gain*60/30.0) + 3, 0, 63);

    //calculate the actual gain value
    if (reg < 4)       gain = 0;
    else if (reg < 48) gain = float(reg/2 - 1);
    else               gain = float(reg/2.0 - 1.5);

    //return register value
    return reg;
}

/*!
 * Convert a requested gain for the tx bb into the integer register value.
 * The gain passed into the function will be set to the actual value.
 * \param gain the requested gain in dB
 * \return gain enum value
 */
static max2829_regs_t::tx_baseband_gain_t gain_to_tx_bb_reg(float &gain){
    int reg = std::clip(boost::math::iround(gain*3/5.0), 0, 3);
    switch(reg){
    case 0:
        gain = 0;
        return max2829_regs_t::TX_BASEBAND_GAIN_0DB;
    case 1:
        gain = 2;
        return max2829_regs_t::TX_BASEBAND_GAIN_2DB;
    case 2:
        gain = 3.5;
        return max2829_regs_t::TX_BASEBAND_GAIN_3_5DB;
    case 3:
        gain = 5;
        return max2829_regs_t::TX_BASEBAND_GAIN_5DB;
    }
    BOOST_THROW_EXCEPTION(std::runtime_error("should not get here"));
    return max2829_regs_t::TX_BASEBAND_GAIN_0DB;
}

/*!
 * Convert a requested gain for the rx vga into the integer register value.
 * The gain passed into the function will be set to the actual value.
 * \param gain the requested gain in dB
 * \return 5 bit the register value
 */
static int gain_to_rx_vga_reg(float &gain){
    int reg = std::clip(boost::math::iround(gain/2.0), 0, 31);
    gain = float(reg*2);
    return reg;
}

/*!
 * Convert a requested gain for the rx lna into the integer register value.
 * The gain passed into the function will be set to the actual value.
 * \param gain the requested gain in dB
 * \return 2 bit the register value
 */
static int gain_to_rx_lna_reg(float &gain){
    int reg = std::clip(boost::math::iround(gain*2/30.5) + 1, 0, 3);
    switch(reg){
    case 0:
    case 1: gain = 0;    break;
    case 2: gain = 15;   break;
    case 3: gain = 30.5; break;
    }
    return reg;
}

void xcvr2450::set_tx_gain(float gain, const std::string &name){
    assert_has(xcvr_tx_gain_ranges.keys(), name, "xcvr tx gain name");
    if (name == "VGA"){
        _max2829_regs.tx_vga_gain = gain_to_tx_vga_reg(gain);
        send_reg(0xC);
    }
    else if(name == "BB"){
        _max2829_regs.tx_baseband_gain = gain_to_tx_bb_reg(gain);
        send_reg(0x9);
    }
    else UHD_ASSERT_THROW(false);
    _tx_gains[name] = gain;
}

void xcvr2450::set_rx_gain(float gain, const std::string &name){
    assert_has(xcvr_rx_gain_ranges.keys(), name, "xcvr rx gain name");
    if (name == "VGA"){
        _max2829_regs.rx_vga_gain = gain_to_rx_vga_reg(gain);
        send_reg(0xB);
    }
    else if(name == "LNA"){
        _max2829_regs.rx_lna_gain = gain_to_rx_lna_reg(gain);
        send_reg(0xB);
    }
    else UHD_ASSERT_THROW(false);
    _rx_gains[name] = gain;
}

/***********************************************************************
 * RX Get and Set
 **********************************************************************/
void xcvr2450::rx_get(const wax::obj &key_, wax::obj &val){
    wax::obj key; std::string name;
    boost::tie(key, name) = extract_named_prop(key_);

    //handle the get request conditioned on the key
    switch(key.as<subdev_prop_t>()){
    case SUBDEV_PROP_NAME:
        val = get_rx_id().to_pp_string();
        return;

    case SUBDEV_PROP_OTHERS:
        val = prop_names_t(); //empty
        return;

    case SUBDEV_PROP_GAIN:
        assert_has(_rx_gains.keys(), name, "xcvr rx gain name");
        val = _rx_gains[name];
        return;

    case SUBDEV_PROP_GAIN_RANGE:
        assert_has(xcvr_rx_gain_ranges.keys(), name, "xcvr rx gain name");
        val = xcvr_rx_gain_ranges[name];
        return;

    case SUBDEV_PROP_GAIN_NAMES:
        val = prop_names_t(xcvr_rx_gain_ranges.keys());
        return;

    case SUBDEV_PROP_FREQ:
        val = _lo_freq;
        return;

    case SUBDEV_PROP_FREQ_RANGE:
        val = xcvr_freq_range;
        return;

    case SUBDEV_PROP_ANTENNA:
        val = _rx_ant;
        return;

    case SUBDEV_PROP_ANTENNA_NAMES:
        val = xcvr_antennas;
        return;

    case SUBDEV_PROP_QUADRATURE:
        val = true;
        return;

    case SUBDEV_PROP_IQ_SWAPPED:
        val = false;
        return;

    case SUBDEV_PROP_SPECTRUM_INVERTED:
        val = false;
        return;

    case SUBDEV_PROP_USE_LO_OFFSET:
        val = false;
        return;

    case SUBDEV_PROP_LO_LOCKED:
        val = this->get_locked();
        return;

    case SUBDEV_PROP_RSSI:
        val = this->get_rssi();
        return;

    default: UHD_THROW_PROP_GET_ERROR();
    }
}

void xcvr2450::rx_set(const wax::obj &key_, const wax::obj &val){
    wax::obj key; std::string name;
    boost::tie(key, name) = extract_named_prop(key_);

    //handle the get request conditioned on the key
    switch(key.as<subdev_prop_t>()){

    case SUBDEV_PROP_FREQ:
        this->set_lo_freq(val.as<double>());
        return;

    case SUBDEV_PROP_GAIN:
        this->set_rx_gain(val.as<float>(), name);
        return;

    case SUBDEV_PROP_ANTENNA:
        this->set_rx_ant(val.as<std::string>());
        return;

    default: UHD_THROW_PROP_SET_ERROR();
    }
}

/***********************************************************************
 * TX Get and Set
 **********************************************************************/
void xcvr2450::tx_get(const wax::obj &key_, wax::obj &val){
    wax::obj key; std::string name;
    boost::tie(key, name) = extract_named_prop(key_);

    //handle the get request conditioned on the key
    switch(key.as<subdev_prop_t>()){
    case SUBDEV_PROP_NAME:
        val = get_tx_id().to_pp_string();
        return;

    case SUBDEV_PROP_OTHERS:
        val = prop_names_t(); //empty
        return;

    case SUBDEV_PROP_GAIN:
        assert_has(_tx_gains.keys(), name, "xcvr tx gain name");
        val = _tx_gains[name];
        return;

    case SUBDEV_PROP_GAIN_RANGE:
        assert_has(xcvr_tx_gain_ranges.keys(), name, "xcvr tx gain name");
        val = xcvr_tx_gain_ranges[name];
        return;

    case SUBDEV_PROP_GAIN_NAMES:
        val = prop_names_t(xcvr_tx_gain_ranges.keys());
        return;

    case SUBDEV_PROP_FREQ:
        val = _lo_freq;
        return;

    case SUBDEV_PROP_FREQ_RANGE:
        val = xcvr_freq_range;
        return;

    case SUBDEV_PROP_ANTENNA:
        val = _tx_ant;
        return;

    case SUBDEV_PROP_ANTENNA_NAMES:
        val = xcvr_antennas;
        return;

    case SUBDEV_PROP_QUADRATURE:
        val = true;
        return;

    case SUBDEV_PROP_IQ_SWAPPED:
        val = true;
        return;

    case SUBDEV_PROP_SPECTRUM_INVERTED:
        val = false;
        return;

    case SUBDEV_PROP_USE_LO_OFFSET:
        val = false;
        return;

    case SUBDEV_PROP_LO_LOCKED:
        val = this->get_locked();
        return;

    default: UHD_THROW_PROP_GET_ERROR();
    }
}

void xcvr2450::tx_set(const wax::obj &key_, const wax::obj &val){
    wax::obj key; std::string name;
    boost::tie(key, name) = extract_named_prop(key_);

    //handle the get request conditioned on the key
    switch(key.as<subdev_prop_t>()){

    case SUBDEV_PROP_FREQ:
        set_lo_freq(val.as<double>());
        return;

    case SUBDEV_PROP_GAIN:
        this->set_tx_gain(val.as<float>(), name);
        return;

    case SUBDEV_PROP_ANTENNA:
        this->set_tx_ant(val.as<std::string>());
        return;

    default: UHD_THROW_PROP_SET_ERROR();
    }
}
