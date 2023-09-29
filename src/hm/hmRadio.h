//-----------------------------------------------------------------------------
// 2023 Ahoy, https://github.com/lumpapu/ahoy
// Creative Commons - http://creativecommons.org/licenses/by-nc-sa/4.0/deed
//-----------------------------------------------------------------------------

#ifndef __RADIO_H__
#define __RADIO_H__

#include "../utils/dbg.h"
#include <RF24.h>
#include "../utils/crc.h"
#include "../config/config.h"
#include "SPI.h"

#define SPI_SPEED           1000000

//#define RF_CHANNELS         5

#define TX_REQ_INFO         0x15
#define TX_REQ_DEVCONTROL   0x51
#define ALL_FRAMES          0x80
#define SINGLE_FRAME        0x81

//#define AHOY_RADIO_TX_PENDING_LOOP

const char* const rf24AmpPowerNames[] = {"MIN", "LOW", "HIGH", "MAX"};

// Depending on the program, the module can work on 2403, 2423, 2440, 2461 or 2475MHz.
// Channel List      2403, 2423, 2440, 2461, 2475MHz
const uint8_t rf24ChLst[RF_CHANNELS] = { 3, 23, 40, 61, 75 };

// default rx channel hopping
const uint8_t rf24RxDefChan[5][5] = {
    { 40, 61, 75,  3, 23 },
    { 61, 75,  3, 23, 40 },
    { 75,  3, 23, 40, 61 },
    {  3, 23, 40, 61, 75 },
    { 23, 40, 61, 75, 03 }
};

// reduced rx channel hopping for HM Inverter with only 1 Input (HM300 testet)
const uint8_t rf24RxHMCh1[5][5] = { // same formal array size for each variant
    { 40, 61 },
    { 61, 75 },
    { 75, 75 /* 3 */ }, /* some strange problems with rx 3 especially here, so take 75 doubled instead */
    {  3, 23 },
    { 23, 40 }
};


//-----------------------------------------------------------------------------
// HM Radio class
//-----------------------------------------------------------------------------
template <uint8_t IRQ_PIN = DEF_NRF_IRQ_PIN, uint8_t CE_PIN = DEF_NRF_CE_PIN, uint8_t CS_PIN = DEF_NRF_CS_PIN, uint8_t AMP_PWR = RF24_PA_LOW, uint8_t SCLK_PIN = DEF_NRF_SCLK_PIN, uint8_t MOSI_PIN = DEF_NRF_MOSI_PIN, uint8_t MISO_PIN = DEF_NRF_MISO_PIN>
class HmRadio {
    public:
        HmRadio() : mNrf24(CE_PIN, CS_PIN, SPI_SPEED) {
            if(mSerialDebug) {
                DPRINT(DBG_VERBOSE, F("hmRadio.h : HmRadio():mNrf24(CE_PIN: "));
                DBGPRINT(String(CE_PIN));
                DBGPRINT(F(", CS_PIN: "));
                DBGPRINT(String(CS_PIN));
                DBGPRINT(F(", SPI_SPEED: "));
                DBGPRINT(String(SPI_SPEED));
                DBGPRINTLN(F(")"));
            }

            // default channels
            mTxChIdx    = AHOY_RF24_DEF_TX_CHANNEL;
            mRxChIdx    = AHOY_RF24_DEF_RX_CHANNEL;

            mSerialDebug    = false;
            mIrqRcvd        = false;
        }
        ~HmRadio() {}

        void setup(statistics_t *stat, uint8_t ampPwr = RF24_PA_LOW, uint8_t irq = IRQ_PIN, uint8_t ce = CE_PIN, uint8_t cs = CS_PIN, uint8_t sclk = SCLK_PIN, uint8_t mosi = MOSI_PIN, uint8_t miso = MISO_PIN) {
            DPRINTLN(DBG_VERBOSE, F("hmRadio.h:setup"));
            pinMode(irq, INPUT_PULLUP);
            mStat = stat;

            uint32_t dtuSn = 0x87654321;
            uint32_t chipID = 0; // will be filled with last 3 bytes of MAC
            #ifdef ESP32
            uint64_t MAC = ESP.getEfuseMac();
            chipID = ((MAC >> 8) & 0xFF0000) | ((MAC >> 24) & 0xFF00) | ((MAC >> 40) & 0xFF);
            #else
            chipID = ESP.getChipId();
            #endif
            if(chipID) {
                dtuSn = 0x80000000; // the first digit is an 8 for DTU production year 2022, the rest is filled with the ESP chipID in decimal
                for(int i = 0; i < 7; i++) {
                    dtuSn |= (chipID % 10) << (i * 4);
                    chipID /= 10;
                }
            }
            // change the byte order of the DTU serial number and append the required 0x01 at the end
            DTU_RADIO_ID = ((uint64_t)(((dtuSn >> 24) & 0xFF) | ((dtuSn >> 8) & 0xFF00) | ((dtuSn << 8) & 0xFF0000) | ((dtuSn << 24) & 0xFF000000)) << 8) | 0x01;

            #ifdef ESP32
                #if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
                    mSpi = new SPIClass(HSPI);
                #else
                    mSpi = new SPIClass(VSPI);
                #endif
                mSpi->begin(sclk, miso, mosi, cs);
            #else
                //the old ESP82xx cannot freely place their SPI pins
                mSpi = new SPIClass();
                mSpi->begin();
            #endif
            mNrf24.begin(mSpi, ce, cs);
            mNrf24.setRetries(3, 15); // 3*250us + 250us and 15 loops -> 15ms

            mNrf24.setChannel(rf24RxDefChan[AHOY_RF24_DEF_TX_CHANNEL][AHOY_RF24_DEF_RX_CHANNEL]);
            mNrf24.startListening();
            mNrf24.setDataRate(RF24_250KBPS);
            mNrf24.setAutoAck(true);
            mNrf24.enableDynamicAck();
            mNrf24.enableDynamicPayloads();
            mNrf24.setCRCLength(RF24_CRC_16);
            mNrf24.setAddressWidth(5);
            mNrf24.openReadingPipe(1, reinterpret_cast<uint8_t*>(&DTU_RADIO_ID));

            // enable all receiving interrupts
            mNrf24.maskIRQ(false, false, false);

            DPRINT(DBG_INFO, F("RF24 Amp Pwr: RF24_PA_"));
            DPRINTLN(DBG_INFO, String(rf24AmpPowerNames[ampPwr]));
            mNrf24.setPALevel(ampPwr & 0x03);

            if(mNrf24.isChipConnected()) {
                DPRINTLN(DBG_INFO, F("Radio Config:"));
                mNrf24.printPrettyDetails();
                DPRINT(DBG_INFO, F("DTU_SN: 0x"));
                DBGPRINTLN(String(DTU_RADIO_ID, HEX));
            } else
                DPRINTLN(DBG_WARN, F("WARNING! your NRF24 module can't be reached, check the wiring"));
        }

        bool loop(void) {
#ifdef AHOY_RADIO_TX_PENDING_LOOP
            if (!mTxPending) {
                return false;
            }
            mTxPending = false;
            while (!mIrqRcvd) {
                yield();
            }
#else
            if (!mIrqRcvd)
                return false; // nothing to do
#endif
            mIrqRcvd = false;
            bool tx_ok, tx_fail, rx_ready;
            mNrf24.whatHappened(tx_ok, tx_fail, rx_ready);  // resets the IRQ pin to HIGH
            mNrf24.flush_tx();                              // empty TX FIFO

            // start listening
            mNrf24.setChannel(mRxChannels[mRxChIdx]);
            mNrf24.startListening();

            uint32_t startMicros = micros();
            uint32_t loopMillis = millis();
            while (millis()-loopMillis < mRxAnswerTmo) {
                while (micros()-startMicros < mRxChanTmo) {  // listen (4088us or?) 5110us to each channel
                        if (mIrqRcvd) {
                        mIrqRcvd = false;
                        if (getReceived()) {        // everything received
                            return true;
                        }
                    }
                    yield();
                }
                // switch to next RX channel
                startMicros = micros();
                if(++mRxChIdx >= mMaxRxChannels)
                    mRxChIdx = 0;
                mNrf24.setChannel(mRxChannels[mRxChIdx]);
                yield();
            }
            // not finished but time is over
            return true;
        }

        void handleIntr(void) {
            mIrqRcvd = true;
        }

        bool isChipConnected(void) {
            //DPRINTLN(DBG_VERBOSE, F("hmRadio.h:isChipConnected"));
            return mNrf24.isChipConnected();
        }
        void enableDebug() {
            mSerialDebug = true;
        }

        void sendControlPacket(uint64_t invId, inv_type_t invType, uint8_t txChan, uint8_t cmd, uint16_t *data, bool isRetransmit, bool isNoMI = true, uint16_t powerMax = 0) {
            DPRINT(DBG_INFO, F("sendControlPacket cmd: 0x"));
            DBGHEXLN(cmd);
            prepareReceive(invType, txChan, 1);
            initPacket(invId, TX_REQ_DEVCONTROL, SINGLE_FRAME);
            uint8_t cnt = 10;
            if (isNoMI) {
                mTxBuf[cnt++] = cmd; // cmd -> 0 on, 1 off, 2 restart, 11 active power, 12 reactive power, 13 power factor
                mTxBuf[cnt++] = 0x00;
                if(cmd >= ActivePowerContr && cmd <= PFSet) { // ActivePowerContr, ReactivePowerContr, PFSet
                    mTxBuf[cnt++] = ((data[0] * 10) >> 8) & 0xff; // power limit
                    mTxBuf[cnt++] = ((data[0] * 10)     ) & 0xff; // power limit
                    mTxBuf[cnt++] = ((data[1]     ) >> 8) & 0xff; // setting for persistens handlings
                    mTxBuf[cnt++] = ((data[1]     )     ) & 0xff; // setting for persistens handling
                }
            } else { //MI 2nd gen. specific
                switch (cmd) {
                    case Restart:
                    case TurnOn:
                        //mTxBuf[0] = 0x50;
                        mTxBuf[9] = 0x55;
                        mTxBuf[10] = 0xaa;
                        break;
                    case TurnOff:
                        mTxBuf[9] = 0xaa;
                        mTxBuf[10] = 0x55;
                        break;
                    case ActivePowerContr:
                        if (data[1]<256) { // non persistent
                            mTxBuf[9] = 0x5a;
                            mTxBuf[10] = 0x5a;
                            //Testing only! Original NRF24_DTUMIesp.ino code #L612-L613:
                            //UsrData[0]=0x5A;UsrData[1]=0x5A;UsrData[2]=100;//0x0a;// 10% limit
                            //UsrData[3]=((Limit*10) >> 8) & 0xFF;   UsrData[4]= (Limit*10)  & 0xFF;   //WR needs 1 dec= zB 100.1 W
                            if (!data[1]) {   //     AbsolutNonPersistent
                                mTxBuf[++cnt] = 100; //10% limit, seems to be necessary to send sth. at all, but for MI-1500 this has no effect
                                //works (if ever!) only for absulute power limits!
                                mTxBuf[++cnt] = ((data[0] * 10) >> 8) & 0xff; // power limit in W
                                mTxBuf[++cnt] = ((data[0] * 10)     ) & 0xff; // power limit in W
                            } else if (powerMax) {       //relative, but 4ch-MI (if ever) only accepts absolute values
                                mTxBuf[++cnt] = data[0]; // simple power limit in %, might be necessary to multiply by 10?
                                mTxBuf[++cnt] = ((data[0] * 10 * powerMax) >> 8) & 0xff; // power limit
                                mTxBuf[++cnt] = ((data[0] * 10 * powerMax)     ) & 0xff; // power limit
                            } else {   // might work for 1/2ch MI (if ever)
                                mTxBuf[++cnt] = data[0]; // simple power limit in %, might be necessary to multiply by 10?
                            }
                        } else {       // persistent power limit needs to be translated in DRED command (?)
                            /* DRED instruction
                            Order	 Function
                            0x55AA	 Boot without DRM restrictions
                            0xA5A5	 DRM0 shutdown
                            0x5A5A	 DRM5 power limit 0%
                            0xAA55	 DRM6 power limit 50%
                            0x5A55	 DRM8 unlimited power operation
                            */
                            mTxBuf[0] = 0x50;

                            if (data[1] == 256UL) {   //     AbsolutPersistent
                                if (data[0] == 0 && !powerMax) {
                                    mTxBuf[9]  = 0xa5;
                                    mTxBuf[10] = 0xa5;
                                } else if (data[0] == 0 || !powerMax || data[0] < powerMax/4 ) {
                                    mTxBuf[9]  = 0x5a;
                                    mTxBuf[10] = 0x5a;
                                } else if (data[0] <=  powerMax/4*3) {
                                    mTxBuf[9]  = 0xaa;
                                    mTxBuf[10] = 0x55;
                                } else if (data[0] <=  powerMax) {
                                    mTxBuf[9]  = 0x5a;
                                    mTxBuf[10] = 0x55;
                                } else if (data[0] > powerMax*2) {
                                    mTxBuf[9]  = 0x55;
                                    mTxBuf[10] = 0xaa;
                                }
                            }
                        }
                        break;
                    default:
                        return;
                }
                cnt++;
            }
            sendPacket(invId, txChan, cnt, isRetransmit, isNoMI);
        }

        void prepareDevInformCmd(uint64_t invId, inv_type_t invType, uint8_t txChan, uint8_t cmd, uint32_t ts, uint16_t alarmMesId, bool isRetransmit, uint8_t reqfld=TX_REQ_INFO) { // might not be necessary to add additional arg.
            if(mSerialDebug) {
                DPRINT(DBG_DEBUG, F("prepareDevInformCmd 0x"));
                DPRINTLN(DBG_DEBUG,String(cmd, HEX));
            }
            uint8_t rxFrameCnt = MAX_PAYLOAD_ENTRIES ;
            if (cmd == RealTimeRunData_Debug) {
                rxFrameCnt = invType+1;
            } else if( (cmd == InverterDevInform_All) || (cmd == SystemConfigPara) || (GetLossRate)) {
                rxFrameCnt = 1;
            }

            prepareReceive(invType, txChan, rxFrameCnt);
            initPacket(invId, reqfld, ALL_FRAMES);
            mTxBuf[10] = cmd; // cid
            mTxBuf[11] = 0x00;
            CP_U32_LittleEndian(&mTxBuf[12], ts);
            if (cmd == AlarmData ) { //cmd == RealTimeRunData_Debug ||
                mTxBuf[18] = (alarmMesId >> 8) & 0xff;
                mTxBuf[19] = (alarmMesId     ) & 0xff;
            }
            sendPacket(invId, txChan, 24, isRetransmit);
        }

        void sendCmdPacket(uint64_t invId, inv_type_t invType, uint8_t txChan, uint8_t mid, uint8_t pid, bool isRetransmit, bool appendCrc16=true) {
            prepareReceive(invType, txChan, 1);
            initPacket(invId, mid, pid);
            sendPacket(invId, txChan, 10, isRetransmit, appendCrc16);
        }

        uint8_t getDataRate(void) {
            if(!mNrf24.isChipConnected())
                return 3; // unknown
            return mNrf24.getDataRate();
        }

        bool isPVariant(void) {
            return mNrf24.isPVariant();
        }

        std::queue<packet_t> mBufCtrl;
        bool mSerialDebug;

    private:
         void prepareReceive(inv_type_t invType, uint8_t txChan, uint8_t rxFrameCnt) {
            if (invType) { // not INV_TYPE_DEFAULT
                mRxAnswerTmo = rxFrameCnt * (RX_WAIT_SFR_TMO + RX_WAIT_SAFETY_MRGN); // current formula for hm inverters
                if (mRxAnswerTmo > RX_ANSWER_TMO) {
                    mRxAnswerTmo = RX_ANSWER_TMO;
                }

                if (invType == INV_TYPE_HMCH1) {
                    mRxChannels = (uint8_t *)(rf24RxHMCh1[txChan]);
                    mMaxRxChannels = RX_HMCH1_MAX_CHANNELS;
                    mRxChanTmo = RX_CHAN_MHCH1_TMO;
                } else {
                    mRxChanTmo = RX_CHAN_TMO; // no change
                    mRxChannels = (uint8_t *)(rf24RxDefChan[txChan]); // no change
                    mMaxRxChannels = RX_DEF_MAX_CHANNELS; // no change
                }

            } else {
                mRxChannels = (uint8_t *)(rf24RxDefChan[txChan]);
                mMaxRxChannels = RX_DEF_MAX_CHANNELS;
                mRxAnswerTmo = RX_ANSWER_TMO;
                mRxChanTmo = RX_CHAN_TMO;
            }
        }

        bool getReceived(void) {
            bool tx_ok, tx_fail, rx_ready;
            mNrf24.whatHappened(tx_ok, tx_fail, rx_ready); // resets the IRQ pin to HIGH

            bool isLastPackage = false;
            while(mNrf24.available()) {
                uint8_t len;
                len = mNrf24.getDynamicPayloadSize(); // if payload size > 32, corrupt payload has been flushed
                if (len > 0) {
                    packet_t p;
                    p.ch = mRxChannels[mRxChIdx];
                    p.len = (len > MAX_RF_PAYLOAD_SIZE) ? MAX_RF_PAYLOAD_SIZE : len;
                    p.rssi = mNrf24.testRPD() ? -64 : -75;
                    mNrf24.read(p.packet, p.len);
                    if (p.packet[0] != 0x00) {
                        mBufCtrl.push(p);
                        if (p.packet[0] == (TX_REQ_INFO + ALL_FRAMES))  // response from get information command
                                isLastPackage = (p.packet[9] > ALL_FRAMES); // > ALL_FRAMES indicates last packet received
                        else if (p.packet[0] == ( 0x0f + ALL_FRAMES) )  // response from MI get information command
                                isLastPackage = (p.packet[9] > 0x10);       // > 0x10 indicates last packet received
                            else if ((p.packet[0] != 0x88) && (p.packet[0] != 0x92)) // ignore fragment number zero and MI status messages //#0 was p.packet[0] != 0x00 &&
                            isLastPackage = true;                       // response from dev control command
                    }
                }
                yield();
            }
            return isLastPackage;
        }

        void initPacket(uint64_t invId, uint8_t mid, uint8_t pid) {
            if(mSerialDebug) {
                DPRINT(DBG_VERBOSE, F("initPacket, mid: "));
                DPRINT(DBG_VERBOSE, String(mid, HEX));
                DPRINT(DBG_VERBOSE,F(" pid: "));
                DPRINTLN(DBG_VERBOSE,String(pid, HEX));
            }
            memset(mTxBuf, 0, MAX_RF_PAYLOAD_SIZE);
            mTxBuf[0] = mid; // message id
            CP_U32_BigEndian(&mTxBuf[1], (invId  >> 8));
            CP_U32_BigEndian(&mTxBuf[5], (DTU_RADIO_ID >> 8));
            mTxBuf[9]  = pid;
        }

        void sendPacket(uint64_t invId, uint8_t rf_ch, uint8_t len, bool isRetransmit, bool appendCrc16=true) {
            //DPRINTLN(DBG_VERBOSE, F("hmRadio.h:sendPacket"));
            //DPRINTLN(DBG_VERBOSE, "sent packet: #" + String(mStat->txCnt));

            // append crc's
            if (appendCrc16 && (len > 10)) {
                // crc control data
                uint16_t crc = ah::crc16(&mTxBuf[10], len - 10);
                mTxBuf[len++] = (crc >> 8) & 0xff;
                mTxBuf[len++] = (crc     ) & 0xff;
            }
            // crc over all
            mTxBuf[len] = ah::crc8(mTxBuf, len);
            len++;

            // set TX and RX channels
            mTxChIdx = rf_ch;
            mRxChIdx = 0;

            if(mSerialDebug) {
                DPRINT(DBG_INFO, F("TX "));
                DBGPRINT(String(len));
                DBGPRINT(" CH");
                DBGPRINT(String(rf24ChLst[mTxChIdx]));
                DBGPRINT(F(" | "));
                ah::dumpBuf(mTxBuf, len);
            }

            mNrf24.stopListening();
            mNrf24.setChannel(rf24ChLst[mTxChIdx]);
            mNrf24.openWritingPipe(reinterpret_cast<uint8_t*>(&invId));
            mNrf24.startWrite(mTxBuf, len, false); // false = request ACK response

#ifdef AHOY_RADIO_TX_PENDING_LOOP
            mTxPending = true;
#endif
            if(isRetransmit)
                mStat->retransmits++;
            else
                mStat->txCnt++;
        }

        volatile bool mIrqRcvd;
        uint64_t DTU_RADIO_ID;

        uint8_t  mRxChIdx;           // cur index in mRxChannels
        uint8_t  mTxChIdx;
        uint8_t  *mRxChannels;       // rx channel to be used; depends on inverter and previous tx channel
        uint8_t  mMaxRxChannels;     // actual size of mRxChannels; depends on inverter and previous tx channel
        uint32_t mRxAnswerTmo;       // max wait time in millis for answers of inverter
        uint32_t mRxChanTmo;         // max wait time in micros for a rx channel

        SPIClass* mSpi;
        RF24 mNrf24;
        uint8_t mTxBuf[MAX_RF_PAYLOAD_SIZE];
        statistics_t *mStat;

#ifdef AHOY_RADIO_TX_PENDING_LOOP
        bool mTxPending = false;            // send has been started: wait in loop to setup receive without break
#endif
};

#endif /*__RADIO_H__*/
